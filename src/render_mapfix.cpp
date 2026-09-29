// M4-P1 map_adjustment ports: case-fix (CString::ToUpper/ToLower
// escape-skip, Win proc1A/B) and split-fix (AddNameArea wide-spacing loop,
// Win proc2V137) + the DIAG-only case probes. (Split of the former
// render.cpp monolith, 2026-09-28.)
#include "render_internal.h"

#include "font.h"
#include "injector.hpp"
#include "wrapfix.h"
#include "log.h"
#include "stubgen.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace eu4cjk::render {
// CString::ToUpper/ToLower loop-body fix (Win mapAdjustmentProc1A/B port):
// the map-name pipeline uppercases province/country names byte-by-byte,
// which mangles escape payloads ('r'->'R' turns 牙 0x7259 into 0x5259 ->
// missing glyph; 米/明 likewise). Stub: escape prefix (0x10..0x13) in al ->
// skip 2 payload bytes via ebp (loop's own inc makes +3 total) and keep the
// prefix byte; otherwise run the original conv call. Hand-assembled, uses
// mov r11,imm64 + call/jmp r11 so no rel32 fixups are needed (r11 is
// call-clobbered anyway).
// DIAG-only: ToUpper entry probe. Country names are built through
// AddNameArea -> CString::ToUpper regardless of render zoom, so sampling
// here catches map-name strings at generation time. CString layout:
// [this+0] = char* buf, [this+8] = length.
static void case_probe_impl(void* self, char tag);

extern "C" void eu4cjk_case_probe(void* self) { case_probe_impl(self, 'U'); }
extern "C" void eu4cjk_case_probe_lo(void* self) { case_probe_impl(self, 'L'); }

static void case_probe_impl(void* self, char tag)
{
    if (!self) return;
    const char* buf = *reinterpret_cast<const char* const*>(self);
    if (!buf) return;
    const auto b0 = static_cast<uint8_t>(buf[0]);
    if (b0 < 0x10 || b0 > 0x13) return;
    static std::atomic<uint64_t> seen[8192];
    static std::atomic<uint32_t> n_ent{0};
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 12; i++) {
        h ^= static_cast<uint8_t>(buf[i]);
        h *= 1099511628211ull;
        if (!buf[i]) break;
    }
    uint32_t n = n_ent.load(std::memory_order_relaxed);
    if (n >= 8192) return;
    for (uint32_t i = 0; i < n; i++)
        if (seen[i].load(std::memory_order_relaxed) == h) return;
    seen[n].store(h, std::memory_order_relaxed);
    n_ent.compare_exchange_strong(n, n + 1, std::memory_order_relaxed);
    char out[140];
    size_t k = 0;
    for (int i = 0; i < 40 && k + 4 < sizeof(out); i++) {
        k += static_cast<size_t>(snprintf(out + k, sizeof(out) - k, "%02x ",
                                          static_cast<uint8_t>(buf[i])));
        if (!buf[i]) break;
    }
    log_line("[eu4cjk] casestr%c %s\n", tag, out);
}

bool install_case_probe_at(const char* name, uintptr_t entry, uintptr_t resume,
                           void (*probe)(void*))
{
    static const uint8_t orig[7] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50};
    stubgen::StubSpec spec;
    spec.args[0].kind = stubgen::StubSpec::Arg::RegSrc;
    spec.args[0].src = stubgen::Reg::Rdi;
    spec.fn = reinterpret_cast<uintptr_t>(probe);
    spec.save_volatile = true;
    spec.replay = orig;
    spec.replay_len = sizeof(orig);
    spec.resume = resume;
    uintptr_t stub = stubgen::emit(spec);
    if (!stub || !stubgen::install_jmp(entry, orig, sizeof(orig), stub)) {
        log_line("[eu4cjk] case-probe %s: install failed\n", name);
        return false;
    }
    log_line("[eu4cjk] case-probe %s: stub @ 0x%lx\n", name,
             static_cast<unsigned long>(stub));
    return true;
}
// Win mapAdjustmentProc2V137 port: AddNameArea's wide-tracking split loop
// (0x1b5de2a) re-appends each byte as an independent "character" separated
// by spaces, tearing 3-byte CJK escapes apart; the spaced string then
// REPLACES the name (_M_replace), so over-width names render as raw
// payload ("y e f" for 奥斯曼). Stubs:
//  - loop body: escape prefix with payload in bounds -> append all 3 bytes
//    as one unit (own operator+= calls incl. the separator, own cursor
//    +3 with tail handoff; the native loop's inc/jne would runaway).
//  - tail append (0x1b5de6a): skipped when s[len-3] starts a complete
//    escape (the loop already consumed it).
bool install_split_fix()
{
    const uint64_t kPlus  = 0x254c19c;  // CString::operator+=(const char*)
    [[maybe_unused]]  // documented for reference; the split loop only calls the char* overload
    const uint64_t kPlusS = 0x254c16c;  // CString::operator+=(const CString&)
    const uint64_t kLoop  = 0x1b5de2a;  // split-loop body start
    const uint64_t kTail  = 0x1b5de5e;  // tail append entry
    const uint64_t kRes35 = 0x1b5de35;  // normal-char append path
    const uint64_t kDone  = 0x1b5de86;  // tail resume

    uint8_t s[160];
    size_t k = 0;
    auto put = [&](std::initializer_list<uint8_t> bs) { for (uint8_t b : bs) s[k++] = b; };
    auto mov_r11 = [&](uint64_t v) { put({0x49, 0xBB}); std::memcpy(s + k, &v, 8); k += 8; };
    auto jmp_r11 = [&]() { put({0x41, 0xFF, 0xE3}); };
    auto call_r11 = [&]() { put({0x41, 0xFF, 0xD3}); };
    auto nops = [&](size_t n) { for (size_t i = 0; i < n; i++) put({0x90}); };
    const char* sm0 = std::getenv("EU4CJK_SPLIT");
    const bool nocall = sm0 && (std::strcmp(sm0, "nocall") == 0 ||
                                std::strcmp(sm0, "esc2") == 0);
    const bool noesc = sm0 && std::strcmp(sm0, "noesc") == 0;
    const bool esc2 = sm0 && std::strcmp(sm0, "esc2") == 0;

    put({0x43, 0x8A, 0x44, 0x25, 0x00});           // mov 0x0(%r13,%r12,1),%al (replay)
    put({0x3C, 0x10, 0x72, 0x54});                 // cmp $0x10,%al; jb normal(0x5D)
    put({0x3C, 0x13, 0x77, 0x50});                 // cmp $0x13,%al; ja normal
    put({0x49, 0x8D, 0x4C, 0x24, 0x02});           // lea 0x2(%r12),%rcx
    put({0x4C, 0x39, 0xF9});                       // cmp %r15,%rcx  (i+2 vs len-1)
    put({0x77, 0x46});                             // ja normal (payload out of range)
    put({0x48, 0x83, 0xEC, 0x20});                 // sub $0x20,%rsp
    put({0x88, 0x04, 0x24});                       // mov %al,(%rsp)
    put({0x43, 0x8A, 0x44, 0x25, 0x01});           // mov 0x1(%r13,%r12,1),%al
    put({0x88, 0x44, 0x24, 0x01});                 // mov %al,0x1(%rsp)
    put({0x43, 0x8A, 0x44, 0x25, 0x02});           // mov 0x2(%r13,%r12,1),%al
    put({0x88, 0x44, 0x24, 0x02});                 // mov %al,0x2(%rsp)
    put({0xC6, 0x44, 0x24, 0x03, 0x00});           // movb $0,0x3(%rsp)
    put({0x4C, 0x89, 0xF7});                       // mov %r14,%rdi
    put({0x48, 0x89, 0xE6});                       // mov %rsp,%rsi
    if (nocall) nops(13); else { mov_r11(kPlus); call_r11(); }
    put({0x48, 0x83, 0xC4, 0x20});                 // add $0x20,%rsp
    put({0x49, 0x83, 0xC4, 0x02});                 // add $0x2,%r12 (native inc -> +3)
    mov_r11(0x1b5de44ull); jmp_r11();              // -> native separator/inc/jb path
    put({0x88, 0x85, 0x78, 0xFF, 0xFF, 0xFF});     // normal(0x5D): mov %al,-0x88(%rbp)
    mov_r11(kRes35); jmp_r11();                    // -> 0x1b5de35
    const size_t loop_len = k;                     // = 112
    if (noesc) {
        // diagnostic: always take the normal path (jmp to 0x5D at entry)
        s[0] = 0xE9; s[1] = 0x58; s[2] = 0x00; s[3] = 0x00; s[4] = 0x00;
        s[5] = 0x90; s[6] = 0x90; s[7] = 0x90;
    }
    const uintptr_t stub1 = stubgen::emit_raw(s, loop_len);
    if (std::getenv("EU4CJK_DIAG")) {
        char hb[3 * 160 + 8];
        size_t hk = 0;
        for (size_t i = 0; i < loop_len && hk + 4 < sizeof(hb); i++)
            hk += static_cast<size_t>(snprintf(hb + hk, sizeof(hb) - hk,
                                               "%02x ", s[i]));
        log_line("[eu4cjk] split-loop stub bytes: %s\n", hb);
        uintptr_t pa = stub1;
        if (pa) {
            hk = 0;
            const uint8_t* pm = reinterpret_cast<const uint8_t*>(pa);
            for (size_t i = 0; i < loop_len && hk + 4 < sizeof(hb); i++)
                hk += static_cast<size_t>(snprintf(hb + hk, sizeof(hb) - hk,
                                                   "%02x ", pm[i]));
            log_line("[eu4cjk] split-loop page bytes: %s\n", hb);
        }
    }

    k = 0;                                         // tail stub reuses the buffer
    put({0x48, 0x98});                             // cltq (replay)
    put({0x48, 0x83, 0xF8, 0x03});                 // cmp $0x3,%rax
    put({0x72, 0x1C});                             // jb do_append
    put({0x41, 0x8A, 0x4C, 0x05, 0xFD});           // mov -0x3(%r13,%rax,1),%cl
    put({0x80, 0xF9, 0x10});                       // cmp $0x10,%cl
    put({0x72, 0x12});                             // jb do_append
    put({0x80, 0xF9, 0x13});                       // cmp $0x13,%cl
    put({0x77, 0x0D});                             // ja do_append
    mov_r11(kDone); jmp_r11();                     // escape tail: skip append
    put({0x42, 0x8A, 0x44, 0x28, 0xFF});           // do_append: mov -0x1(%rax,%r13,1),%al
    put({0x88, 0x85, 0x78, 0xFF, 0xFF, 0xFF});     // mov %al,-0x88(%rbp)
    put({0x4C, 0x89, 0xF7});                       // mov %r14,%rdi
    put({0x48, 0x8D, 0xB5, 0x78, 0xFF, 0xFF, 0xFF}); // lea -0x88(%rbp),%rsi
    mov_r11(kPlus); call_r11();                    // target += last char
    mov_r11(kDone); jmp_r11();                     // -> 0x1b5de86
    const size_t tail_len = k;                     // = 83
    const uintptr_t stub2 = stubgen::emit_raw(s, tail_len);

    static const uint8_t o1[11] = {
        0x43, 0x8A, 0x44, 0x25, 0x00, 0x88, 0x85, 0x78, 0xFF, 0xFF, 0xFF};
    static const uint8_t o2[28] = {
        0x48, 0x98, 0x42, 0x8A, 0x44, 0x28, 0xFF, 0x88, 0x85, 0x78, 0xFF, 0xFF,
        0xFF, 0x4C, 0x89, 0xF7, 0x48, 0x8D, 0xB5, 0x78, 0xFF, 0xFF, 0xFF, 0xE8,
        0x16, 0xE3, 0x9E, 0x00};

    const char* sm = std::getenv("EU4CJK_SPLIT");
    const bool loop_only = sm && std::strcmp(sm, "loop") == 0;
    const bool tail_only = sm && std::strcmp(sm, "tail") == 0;
    // Loop-exit guard: the escape path advances the cursor by 3 and can
    // overshoot the exact len-1 equality that jne tests. Site semantics:
    // cmp %r12,%r15 (r15 - r12); ja = "r12 < r15 -> continue" exits safely
    // on overshoot (jb would invert the condition and run away).
    {
        uint8_t cur = 0;
        Injector::ReadMemoryRaw(Injector::memory_pointer_raw(
            reinterpret_cast<void*>(0x1b5de5c)), &cur, 1, true);
        if (cur == 0x75) {
            const uint8_t ja = 0x77;
            Injector::WriteMemoryRaw(Injector::memory_pointer_raw(
                reinterpret_cast<void*>(0x1b5de5c)), &ja, 1, true);
            log_line("[eu4cjk] split-fix: loop-exit jne->ja patched\n");
        } else {
            log_line("[eu4cjk] split-fix: exit byte 0x%02x != 0x75, skip ja patch\n",
                     static_cast<unsigned>(cur));
        }
    }
    if (!tail_only) {
        if (!stub1 || !stubgen::install_jmp(kLoop, o1, sizeof(o1), stub1)) {
            log_line("[eu4cjk] split-fix loop: install failed\n");
            return false;
        }
    }
    if (loop_only || esc2) {
        log_line("[eu4cjk] split-fix: loop only (EU4CJK_SPLIT=%s)\n",
                 esc2 ? "esc2" : "loop");
        return true;
    }
    if (!stub2 || !stubgen::install_jmp(kTail + 0xC, o2, sizeof(o2), stub2)) {
        log_line("[eu4cjk] split-fix tail: install failed\n");
        return false;
    }
    if (tail_only)
        log_line("[eu4cjk] split-fix: tail only\n");
    log_line("[eu4cjk] split-fix: loop stub @ 0x%lx, tail stub @ 0x%lx\n",
             static_cast<unsigned long>(stub1), static_cast<unsigned long>(stub2));
    return true;
}
bool install_case_fix(const char* name, uintptr_t site, const uint8_t* orig,
                      uintptr_t resume, uintptr_t conv)
{
    uint8_t s[58];
    size_t k = 0;
    auto put = [&](std::initializer_list<uint8_t> bs) { for (uint8_t b : bs) s[k++] = b; };
    auto mov_r11 = [&](uintptr_t v) { put({0x49, 0xBB}); std::memcpy(s + k, &v, 8); k += 8; };
    put({0x3C, 0x10});                 // 0x00 cmp $0x10,%al
    put({0x72, 0x17});                 // 0x02 jb  0x1B (normal)
    put({0x3C, 0x13});                 // 0x04 cmp $0x13,%al
    put({0x77, 0x13});                 // 0x06 ja  0x1B (normal)
    put({0x83, 0xC5, 0x02});           // 0x08 add $0x2,%ebp   (skip payload)
    mov_r11(resume);                   // 0x0B mov $resume,%r11
    put({0x41, 0xFF, 0xE3});           // 0x15 jmp *%r11
    put({0x90, 0x90, 0x90});           // 0x18 pad -> 0x1B
    put({0x0F, 0xBE, 0xF8});           // 0x1B movsbl %al,%edi (orig site insn)
    mov_r11(conv);                     // 0x1E mov $conv,%r11
    put({0x41, 0xFF, 0xD3});           // 0x28 call *%r11
    put({0x88, 0x03});                 // 0x2B mov %al,(%rbx)  (orig write-back)
    mov_r11(resume);                   // 0x2D mov $resume,%r11
    put({0x41, 0xFF, 0xE3});           // 0x37 jmp *%r11

    const uintptr_t stub = stubgen::emit_raw(s, k);
    if (!stub || !stubgen::install_jmp(site, orig, 10, stub)) {
        log_line("[eu4cjk] case-fix %s: install failed\n", name);
        return false;
    }
    log_line("[eu4cjk] case-fix %s: stub @ 0x%lx (site 0x%lx)\n", name,
             static_cast<unsigned long>(stub), static_cast<unsigned long>(site));
    return true;
}
} // namespace
