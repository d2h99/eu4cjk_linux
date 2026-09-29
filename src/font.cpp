#include "font.h"
#include "log.h"
#include "byte_pattern.h"
#include "injector.hpp"

#include <sys/mman.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace eu4cjk::font {

// Per-font external glyph tables. The mod ships 6 different .fnt files with
// INDEPENDENT atlases: the same id maps to different x/y/w/h in each font.
// A single global table handed every font coordinates from whichever font
// parsed first -> glyphs sampled the wrong atlas -> invisible text (M3-T3
// live finding, 2026-09-26). Mirrors the upstream Windows semantics (the
// inline 256-slot table being expanded lives inside each CBitmapFont).
// Each table: 65536 slots x 8 bytes, calloc'd lazily on first CJK glyph of
// that font (~512KB per mod font). Entries are append-only and published
// with release stores; render-side lookup is lock-free.
namespace {

// Live finding (2026-09-26): the game materialises ~46 CBitmapFont objects
// from the mod's 6 .fnt files (per size/variant/colour), each re-parsing the
// same file. 16 slots overflowed -> rejects exploded (664k). 64 gives headroom
// (~46 observed); on overflow the gate falls back to the original reject path.
constexpr size_t kMaxFonts = 64;

struct FontSlot {
    std::atomic<const void*> font{nullptr};
    std::atomic<uint16_t**> table{nullptr};
    std::atomic<uint32_t> stored{0};
};

FontSlot g_fonts[kMaxFonts];
std::atomic<uint32_t> g_font_count{0};
std::atomic_flag g_font_lock = ATOMIC_FLAG_INIT;
std::atomic<uint32_t> g_stored{0};
std::atomic<uint32_t> g_duplicate{0};

// Lock-free lookup, never allocates (render path: unknown fonts stay null).
uint16_t** table_lookup(const void* font)
{
    for (size_t i = 0; i < kMaxFonts; ++i) {
        const void* f = g_fonts[i].font.load(std::memory_order_acquire);
        if (f == font) return g_fonts[i].table.load(std::memory_order_relaxed);
        if (f == nullptr) break; // append-only: first empty slot = end
    }
    return nullptr;
}

// Gate path: find or create this font's table.
uint16_t** table_for(const void* font)
{
    uint16_t** t = table_lookup(font);
    if (t) return t;
    while (g_font_lock.test_and_set(std::memory_order_acquire)) {}
    t = table_lookup(font); // re-check under lock
    if (!t) {
        for (size_t i = 0; i < kMaxFonts; ++i) {
            if (g_fonts[i].font.load(std::memory_order_relaxed) != nullptr) continue;
            t = static_cast<uint16_t**>(std::calloc(65536, sizeof(uint16_t*)));
            if (t) {
                g_fonts[i].table.store(t, std::memory_order_relaxed);
                g_fonts[i].font.store(font, std::memory_order_release);
                g_fonts[i].stored.store(0, std::memory_order_relaxed);
                g_font_count.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        if (!t) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true))
                log_line("[eu4cjk] glyph gate: FONT REGISTRY FULL (%zu),"
                         " extra fonts keep vanilla reject behaviour\n", kMaxFonts);
        }
    }
    g_font_lock.clear(std::memory_order_release);
    return t;
}

// Linux 1.37.5 reject site inside CBitmapFont::ParseFontFile:
//   204e366: 41 81 fd ff 00 00 00   cmp $0xff,%r13d
//   204e36d: 0f 87 25 01 00 00      ja  204e498   <- taken branch = "id > 0xFF"
// We divert ONLY the taken branch; id <= 0xFF keeps original flow untouched.
constexpr uintptr_t kCmpAddr = 0x204e366;
constexpr uintptr_t kHookAddr = 0x204e36d;   // the ja
constexpr size_t kHookLen = 6;
constexpr uint8_t kOrigBytes[kHookLen] = {0x0F, 0x87, 0x25, 0x01, 0x00, 0x00};
constexpr uintptr_t kLoopHead = 0x204e0aa;   // resume after storing a glyph
constexpr uintptr_t kRejectPath = 0x204e498; // out-of-range ids (>= 0x10000)
constexpr size_t kRelaySize = 4096;

} // namespace

extern "C" int eu4cjk_glyph_gate(const GateArgs* a)
{
    if (a->id >= 65536) return 0; // follow original reject path

    uint16_t** tbl = table_for(a->font);
    if (!tbl) return 0;           // registry full -> original reject path

    uint16_t* node = static_cast<uint16_t*>(std::calloc(8, sizeof(uint16_t)));
    if (!node) return 0;
    for (int i = 0; i < 7; ++i) node[i] = a->f[i];

    if (tbl[a->id] == nullptr) {  // keep-first PER FONT
        tbl[a->id] = node;
        uint32_t n = g_stored.fetch_add(1, std::memory_order_relaxed) + 1;
        uint32_t nf = g_font_count.load(std::memory_order_relaxed);
        for (size_t i = 0; i < kMaxFonts; ++i) {
            if (g_fonts[i].font.load(std::memory_order_relaxed) == a->font) {
                g_fonts[i].stored.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        if (n == 1)
            log_line("[eu4cjk] glyph gate: first external glyph id=0x%X font=%p\n",
                     a->id, a->font);
        else if ((n & 0xFFFF) == 0)  // load-time milestone, 64K steps
            log_line("[eu4cjk] glyph gate: %u external glyphs stored (%u fonts)\n",
                     static_cast<unsigned>(n), static_cast<unsigned>(nf));
    } else {
        std::free(node);
        g_duplicate.fetch_add(1, std::memory_order_relaxed);
    }
    return 1;
}

uint32_t stored_count() { return g_stored.load(std::memory_order_relaxed); }
uint32_t duplicate_count() { return g_duplicate.load(std::memory_order_relaxed); }
uint32_t font_registry_count() { return g_font_count.load(std::memory_order_relaxed); }

// ---- M3-T2: escaped-stream glyph fetch ----------------------------------
// Escape protocol (upstream escape_tool.cpp, decode side, constants verbatim):
//   prefix 0x10: id = (high << 8) | low
//   prefix 0x11: id -= 0x0E
//   prefix 0x12: id += 0x900
//   prefix 0x13: id += 0x8F2
//   out of range (> 0xFFFF) or dead zone (0x100 < id < 0x98F): id = 0x2026
// Inline (non-prefixed) bytes index the engine's 256-slot table directly.
const void* fetch_glyph(const void* font, const void* const* external,
                        const char* p, uint32_t* consumed)
{
    const uint8_t b0 = static_cast<uint8_t>(p[0]);
    if (b0 < 0x10 || b0 > 0x13) {
        *consumed = 1;
        if (!font) return nullptr;
        return *reinterpret_cast<void* const*>(
            static_cast<const uint8_t*>(font) + 0x100 + static_cast<size_t>(b0) * 8);
    }

    const uint32_t id = decode_id(reinterpret_cast<const uint8_t*>(p));
    *consumed = 3;
    if (id <= 0xFF) {
        if (!font) return nullptr;
        return *reinterpret_cast<void* const*>(
            static_cast<const uint8_t*>(font) + 0x100 + static_cast<size_t>(id) * 8);
    }
    return external ? external[id] : nullptr;
}

namespace {

struct Emitter {
    uint8_t* p;
    uint8_t* base;
    void b(uint8_t x) { *p++ = x; }
    // Call right after writing an E9/0F 8x opcode, with p at the rel32 field.
    // Emits rel32 so the instruction (opcode + rel32 = 5 bytes total) lands on target.
    void jmp_rel32(uintptr_t target)
    {
        int32_t rel = static_cast<int32_t>(
            static_cast<int64_t>(target)
            - static_cast<int64_t>(reinterpret_cast<uintptr_t>(base)
                                   + static_cast<size_t>(p - base) + 4));
        std::memcpy(p, &rel, 4);
        p += 4;
    }
    void imm64(uintptr_t v) { std::memcpy(p, &v, 8); p += 8; }
};

void* map_relay_page()
{
    // Must be within +-2GB of the game image (non-PIE @ 0x400000-0x3520000)
    // and must not collide with anything.
    static const uintptr_t candidates[] = {
        0x20000000, 0x30000000, 0x18000000, 0x60000000, 0x10000000,
    };
    for (uintptr_t c : candidates) {
        void* p = mmap(reinterpret_cast<void*>(c), kRelaySize,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) return p;
    }
    return nullptr;
}

size_t emit_relay_stub(uint8_t* page)
{
    Emitter e{page, page};
    e.b(0x55);                          // push rbp            (saves field f5)
    e.b(0x48); e.b(0x89); e.b(0xE5);    // mov  rbp, rsp
    e.b(0x48); e.b(0x83); e.b(0xE4); e.b(0xF0); // and rsp, -16
    e.b(0x48); e.b(0x83); e.b(0xEC); e.b(0x20); // sub rsp, 0x20   (keeps 16-byte alignment for the call; GateArgs needs 24, incl. font @ +0x18)
    e.b(0x44); e.b(0x89); e.b(0x2C); e.b(0x24); // mov [rsp], r13d        id
    e.b(0x4C); e.b(0x8D); e.b(0x55); e.b(0x08); // lea r10, [rbp+8]       entry rsp (scratch)
    e.b(0x41); e.b(0x0F); e.b(0xB7); e.b(0x4A); e.b(0x14); // movzx ecx, [r10+0x14]  f0
    e.b(0x66); e.b(0x89); e.b(0x4C); e.b(0x24); e.b(0x04); // mov [rsp+4], cx
    e.b(0x41); e.b(0x0F); e.b(0xB7); e.b(0x4A); e.b(0x10); // movzx ecx, [r10+0x10]  f1
    e.b(0x66); e.b(0x89); e.b(0x4C); e.b(0x24); e.b(0x06); // mov [rsp+6], cx
    e.b(0x41); e.b(0x0F); e.b(0xB7); e.b(0xCC); // movzx ecx, r12w        f2
    e.b(0x66); e.b(0x89); e.b(0x4C); e.b(0x24); e.b(0x08); // mov [rsp+8], cx
    e.b(0x0F); e.b(0xB7); e.b(0xCB);    // movzx ecx, bx          f3
    e.b(0x66); e.b(0x89); e.b(0x4C); e.b(0x24); e.b(0x0A); // mov [rsp+0xA], cx
    e.b(0x41); e.b(0x0F); e.b(0xB7); e.b(0xCE); // movzx ecx, r14w        f4
    e.b(0x66); e.b(0x89); e.b(0x4C); e.b(0x24); e.b(0x0C); // mov [rsp+0xC], cx
    e.b(0x0F); e.b(0xB7); e.b(0x4D); e.b(0x00); // movzx ecx, [rbp]       f5 (saved)
    e.b(0x66); e.b(0x89); e.b(0x4C); e.b(0x24); e.b(0x0E); // mov [rsp+0xE], cx
    e.b(0x41); e.b(0x0F); e.b(0xB7); e.b(0xCF); // movzx ecx, r15w        f6
    e.b(0x66); e.b(0x89); e.b(0x4C); e.b(0x24); e.b(0x10); // mov [rsp+0x10], cx
    e.b(0x4D); e.b(0x8B); e.b(0x5A); e.b(0x08); // mov r11, [r10+8]      font (this, entry_rsp+8; cf. 0x204e380). REX=4D: W+R(r11)+B(r10)
    e.b(0x4C); e.b(0x89); e.b(0x5C); e.b(0x24); e.b(0x18); // mov [rsp+0x18], r11
    e.b(0x48); e.b(0x89); e.b(0xE7);    // mov rdi, rsp
    e.b(0x48); e.b(0xB8);               // mov rax, imm64
    e.imm64(reinterpret_cast<uintptr_t>(&eu4cjk_glyph_gate));
    e.b(0xFF); e.b(0xD0);               // call rax
    e.b(0x48); e.b(0x89); e.b(0xEC);    // mov rsp, rbp
    e.b(0x5D);                          // pop rbp
    e.b(0x85); e.b(0xC0);               // test eax, eax
    e.b(0x75); e.b(0x05);               // jnz +5  (stored -> loop head)
    // reject path: jmp rel32 kRejectPath
    e.b(0xE9);
    e.jmp_rel32(kRejectPath);
    // stored path: jmp rel32 kLoopHead
    e.b(0xE9);
    e.jmp_rel32(kLoopHead);
    return static_cast<size_t>(e.p - page);
}

} // namespace

bool install_glyph_gate()
{
    auto& bp = BytePattern::temp_instance();
    bp.find_pattern("41 81 FD FF 00 00 00 0F 87 ? ? ? ? 41 83 FD 0A 0F 84");
    if (!bp.has_size(1, "char code point limiter (cmp r13d,0xFF / ja)")) {
        log_line("[eu4cjk] glyph gate: PATTERN NOT FOUND, not installed\n");
        return false;
    }

    uintptr_t cmp_addr = bp.get_first().address();
    if (cmp_addr != kCmpAddr)
        log_line("[eu4cjk] glyph gate: NOTE pattern @ 0x%lx (expected 0x%lx)\n",
                 static_cast<unsigned long>(cmp_addr),
                 static_cast<unsigned long>(kCmpAddr));

    uint8_t orig[kHookLen];
    Injector::ReadMemoryRaw(Injector::memory_pointer_raw(reinterpret_cast<void*>(kHookAddr)), orig,
                            kHookLen, true);
    if (std::memcmp(orig, kOrigBytes, kHookLen) != 0) {
        log_line("[eu4cjk] glyph gate: unexpected bytes at 0x%lx "
                 "(got %02X %02X %02X %02X), abort\n",
                 static_cast<unsigned long>(kHookAddr), orig[0], orig[1], orig[2], orig[3]);
        return false;
    }

    void* relay = map_relay_page();
    if (!relay) {
        log_line("[eu4cjk] glyph gate: cannot map relay page, abort\n");
        return false;
    }
    size_t stub_len = emit_relay_stub(static_cast<uint8_t*>(relay));
    uintptr_t relay_addr = reinterpret_cast<uintptr_t>(relay);

    // Keep the conditional 'ja' (0F 87) opcode, rewrite only its rel32 so the
    // TAKEN branch (id > 0xFF) lands on our relay stub. ids <= 0xFF fall
    // through to the original newline-check + inline-table path untouched.
    uint8_t patch[4];
    int32_t rel = static_cast<int32_t>(static_cast<int64_t>(relay_addr)
                                       - static_cast<int64_t>(kHookAddr + kHookLen));
    std::memcpy(patch, &rel, 4);
    Injector::WriteMemoryRaw(
        Injector::memory_pointer_raw(reinterpret_cast<void*>(kHookAddr + 2)), patch, 4, true);

    log_line("[eu4cjk] glyph gate: installed (cmp @ 0x%lx, relay @ 0x%lx, stub %zu bytes,"
              " per-font external tables)\n",
             static_cast<unsigned long>(kCmpAddr),
             static_cast<unsigned long>(relay_addr), stub_len);

    std::atexit([] {
        log_line("[eu4cjk] glyph gate: final stats: %u stored, %u duplicates, %u fonts\n",
                 static_cast<unsigned>(stored_count()),
                 static_cast<unsigned>(duplicate_count()),
                 static_cast<unsigned>(font_registry_count()));
        for (size_t i = 0; i < kMaxFonts; ++i) {
            const void* f = g_fonts[i].font.load(std::memory_order_relaxed);
            if (!f) break;
            log_line("[eu4cjk] glyph gate: font %zu @ %p: %u glyphs\n", i, f,
                     static_cast<unsigned>(g_fonts[i].stored.load(std::memory_order_relaxed)));
        }
    });
    return true;
}

// Escape combine (upstream decode constants, verbatim). Exposed for logging.
uint32_t decode_id(const uint8_t* p)
{
    const uint32_t low = p[1];
    const uint32_t high = p[2];
    uint32_t id = (high << 8) | low;
    switch (p[0]) {
    case 0x11: id -= 0x0E; break;
    case 0x12: id += 0x900; break;
    case 0x13: id += 0x8F2; break;
    default: break;
    }
    if (id > 0xFFFF || (id < 0x98F && id > 0x100)) id = 0x2026;
    return id;
}

// Live variant for render stubs: resolves external ids through the table of
// the font actually rendering (per-font registry, lock-free lookup; unknown
// fonts resolve to null, matching the engine's empty-slot semantics).
const void* fetch_glyph_live(const void* font, const char* p, uint32_t* consumed)
{
    const uint8_t b0 = static_cast<uint8_t>(p[0]);
    uint32_t id;
    if (b0 < 0x10 || b0 > 0x13) {
        id = b0;
        *consumed = 1;
    } else if (p[1] == 0 || p[2] == 0) {
        // Truncated escape: map-label text transforms can strip payload
        // bytes, leaving a lone prefix (often at the string end). Valid
        // payloads never contain NUL (C-string protocol), and sequential
        // reads stay inside the allocation (p[1] is at most the
        // terminator; p[2] is only read when p[1] is not the terminator).
        // Treat the prefix as a plain byte - no crash, engine fallback box.
        id = b0;
        *consumed = 1;
    } else {
        id = decode_id(reinterpret_cast<const uint8_t*>(p));
        *consumed = 3;
    }
    if (id <= 0xFF) {
        if (!font) return nullptr;
        return *reinterpret_cast<void* const*>(
            static_cast<const uint8_t*>(font) + 0x100 + static_cast<size_t>(id) * 8);
    }
    uint16_t** tbl = table_lookup(font);
    return tbl ? tbl[id] : nullptr;
}

}
