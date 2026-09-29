#include "wrapfix.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "log.h"
#include "stubgen.h"

namespace eu4cjk::wrapfix {
namespace {

uint8_t* g_flags = nullptr;

// fetch site id -> gate bit (-1: site feeds no patched gate).
// Sites: 0=FillVB-A 2=R2T-1 4=R3D-1 6=RTS-1 10=GW (see render.cpp).
constexpr int8_t kSiteToBit[16] = {
    0, -1, 1, -1, 4, -1, 3, -1, -1, -1, 2, -1, -1, -1, -1, -1,
};

struct Gate {
    const char* name;
    uintptr_t region;      // first replaced byte in the engine image
    const uint8_t* orig;   // expected original bytes (install_jmp verifies)
    size_t len;            // region length
    // Stub shape:
    //  entered_on_ne: patch replaced the h!=0 branch (FillVB); the stub sees
    //    only non-space chars: flag==0 -> untaken, flag!=0 -> taken.
    //  else the stub replays cmpw $0,0x6(node): h==0 -> taken; flag!=0 ->
    //    taken; else [replay] -> untaken.
    //  commit_r13: taken path is 'mov %eax,%r13d' then untaken (GW width
    //    commit) instead of a jump.
    bool entered_on_ne;
    uint8_t node_modrm;    // ModRM of cmpw $0,0x6(REG) (unused if entered_on_ne)
    uintptr_t taken;       // break-candidate path (or commit -> untaken for GW)
    uintptr_t untaken;     // native non-space continuation
    const uint8_t* replay; // extra replayed bytes before untaken (may be null)
    size_t replay_len;
    bool commit_r13;
};

// Original bytes verified against eu4.dis (2026-09-28 session):
//  2055619: 66 41 83 7f 06 00 cmpw $0,0x6(%r15) / 205561f: 0f 85 ab 03 00 00 jne 20559d0
//  204f0f6: 66 83 7d 06 00    cmpw $0,0x6(%rbp) / 204f0fb: 74 0d          je  204f10a
//           (fallthrough non-space: 204f0fd: 40 8a ac 24 00 29 00 00 mov 0x2900(%rsp),%bpl
//                                   204f105: e9 ... jmp 204f576)
//  2054098: 66 83 7d 06 00    cmpw $0,0x6(%rbp) / 205409d: 44 0f 44 e8    cmove %eax,%r13d
//           (20540a1: jmp 205419a loop tail)
//  2052239: 66 83 7d 06 00    cmpw $0,0x6(%rbp) / 205223e: 0f 84 86 01 00 00 je 20523ca
//  205488d: 66 83 7b 06 00    cmpw $0,0x6(%rbx) / 2054892: 74 19          je  20548ad
const uint8_t kOrigFVB[6]  = {0x0F, 0x85, 0xAB, 0x03, 0x00, 0x00};
const uint8_t kOrigR2T[7]  = {0x66, 0x83, 0x7D, 0x06, 0x00, 0x74, 0x0D};
const uint8_t kOrigGW[9]   = {0x66, 0x83, 0x7D, 0x06, 0x00, 0x44, 0x0F, 0x44, 0xE8};
const uint8_t kOrigRTS[11] = {0x66, 0x83, 0x7D, 0x06, 0x00, 0x0F, 0x84, 0x86, 0x01, 0x00, 0x00};
const uint8_t kOrigR3D[7]  = {0x66, 0x83, 0x7B, 0x06, 0x00, 0x74, 0x19};
const uint8_t kReplayR2T[8] = {0x40, 0x8A, 0xAC, 0x24, 0x00, 0x29, 0x00, 0x00};
// kReplayR2T = mov 0x2900(%rsp),%bpl (REX + 7), the non-space path's first
// instruction after the gate.

const Gate kGates[kGateBits] = {
    // 0: FillVertexBuffer loop A - jne(h!=0) rewritten; wrap block = fallthrough
    {"FillVB", 0x205561f, kOrigFVB, sizeof(kOrigFVB),
     true, 0, 0x2055625, 0x20559d0, nullptr, 0, false},
    // 1: RenderToTexture wrap loop - replay cmpw + the bpl reload
    {"R2T", 0x204f0f6, kOrigR2T, sizeof(kOrigR2T),
     false, 0x7D, 0x204f10a, 0x204f105, kReplayR2T, 8, false},
    // 2: GetWidthOfString - cmove commit form
    {"GW", 0x2054098, kOrigGW, sizeof(kOrigGW),
     false, 0x7D, 0, 0x20540a1, nullptr, 0, true},
    // 3: RenderToScreen wrap loop
    {"RTS", 0x2052239, kOrigRTS, sizeof(kOrigRTS),
     false, 0x7D, 0x20523ca, 0x2052244, nullptr, 0, false},
    // 4: Render3d wrap loop
    {"R3D", 0x205488d, kOrigR3D, sizeof(kOrigR3D),
     false, 0x7B, 0x20548ad, 0x2054894, nullptr, 0, false},
};

bool fits_rel32(uintptr_t from, uintptr_t to)
{
    const int64_t d = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    return d >= INT32_MIN && d <= INT32_MAX;
}

} // namespace

size_t stub_len(int bit)
{
    static const size_t kLen[kGateBits] = {18, 37, 37, 29, 29};
    if (bit < 0 || bit >= kGateBits) return 0;
    return kLen[bit];
}

size_t build_stub_ex(int bit, uintptr_t flag_addr, uintptr_t stub_addr,
                     uintptr_t taken, uintptr_t untaken, uint8_t* out)
{
    if (bit < 0 || bit >= kGateBits || !out || !stub_addr) return 0;
    const Gate& g = kGates[bit];
    uint8_t s[64];
    size_t k = 0;

    auto put = [&](std::initializer_list<uint8_t> bs) { for (uint8_t b : bs) s[k++] = b; };
    auto put_rel32 = [&](uint8_t op0, uint8_t op1, uintptr_t target) -> bool {
        // two-byte opcode (0F 84/85) or E9 + rel32
        if (op1 == 0xE9u) { s[k++] = 0xE9; }
        else { s[k++] = op0; s[k++] = op1; }
        const uintptr_t from = stub_addr + k + 4;
        if (!fits_rel32(from, target)) return false;
        const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(target)
                                                 - static_cast<int64_t>(from));
        std::memcpy(s + k, &rel, 4);
        k += 4;
        return true;
    };
    auto put_flag_cmp = [&]() -> bool {
        s[k++] = 0x80; s[k++] = 0x3D;
        const uintptr_t from = stub_addr + k + 4 + 1; // disp32 then imm8
        if (!fits_rel32(from, flag_addr)) return false;
        const int32_t disp = static_cast<int32_t>(static_cast<int64_t>(flag_addr)
                                                  - static_cast<int64_t>(from));
        std::memcpy(s + k, &disp, 4);
        k += 4;
        s[k++] = 0x00;
        return true;
    };

    if (!g.entered_on_ne) {
        put({0x66, 0x83, g.node_modrm, 0x06, 0x00});   // cmpw $0,0x6(REG)
        if (g.commit_r13) {
            // je commit (commit sits at fixed offset: len-8)
            if (!put_rel32(0x0F, 0x84, stub_addr + stub_len(bit) - 8)) return 0;
        } else {
            if (!put_rel32(0x0F, 0x84, taken)) return 0;        // je taken
        }
    }
    if (!put_flag_cmp()) return 0;                              // cmpb $0,flag(%rip)
    if (g.entered_on_ne) {
        if (!put_rel32(0x0F, 0x84, untaken)) return 0;          // je  untaken (ASCII)
        if (!put_rel32(0, 0xE9, taken)) return 0;               // jmp taken   (CJK)
    } else if (g.commit_r13) {
        if (!put_rel32(0x0F, 0x85, stub_addr + stub_len(bit) - 8)) return 0; // jne commit
        if (!put_rel32(0, 0xE9, untaken)) return 0;             // jmp resume
        // commit: mov %eax,%r13d. 89 /r puts DEST in rm -> r13 needs REX.B
        // (0x41); REX.R (0x44) would encode mov %r8d,%ebp (gdb-verified trap,
        // opposite polarity of the 0F 44 cmove it replaces).
        put({0x41, 0x89, 0xC5});
        if (!put_rel32(0, 0xE9, untaken)) return 0;             // jmp resume
    } else {
        if (!put_rel32(0x0F, 0x85, taken)) return 0;            // jne taken (CJK)
        for (size_t i = 0; i < g.replay_len; ++i) s[k++] = g.replay[i];
        if (!put_rel32(0, 0xE9, untaken)) return 0;             // jmp resume
    }

    if (k != stub_len(bit)) return 0;  // encoding drift guard
    std::memcpy(out, s, k);
    return k;
}

size_t build_stub(int bit, uintptr_t flag_addr, uintptr_t stub_addr, uint8_t* out)
{
    if (bit < 0 || bit >= kGateBits) return 0;
    const Gate& g = kGates[bit];
    return build_stub_ex(bit, flag_addr, stub_addr, g.taken, g.untaken, out);
}

// Per-site CJK note counts (diagnostics: which fetch sites decode CJK).
std::atomic<uint64_t> g_note_cjk[16];

void note_char(uint32_t site, bool cjk)
{
    if (!g_flags || site >= 16) return;
    const int b = kSiteToBit[site];
    if (b < 0) return;
    if (cjk) g_note_cjk[site].fetch_add(1, std::memory_order_relaxed);
    g_flags[b] = cjk ? 1u : 0u;
}

uint64_t note_cjk_count(uint32_t site)
{
    return site < 16 ? g_note_cjk[site].load(std::memory_order_relaxed) : 0;
}

void bind_flags_for_test(uint8_t* flags) { g_flags = flags; }

bool install()
{
    static bool done = false;
    if (done) return true;
    done = true;

    if (std::getenv("EU4CJK_WRAP0")) {
        log_line("[eu4cjk] wrap-fix: disabled (EU4CJK_WRAP0)\n");
        return true;
    }
    uint32_t mask = kDefaultMask;
    const char* m = std::getenv("EU4CJK_WRAP_MASK");
    if (m) mask = static_cast<uint32_t>(std::strtoul(m, nullptr, 0));
    if (mask & ~0xFFu)
        log_line("[eu4cjk] wrap-fix: bits 5-7 (map paths) are reserved,"
                 " not installed\n");
    mask &= 0x1Fu;

    uint8_t* flags = static_cast<uint8_t*>(stubgen::reserve_data(16));
    if (!flags) {
        log_line("[eu4cjk] wrap-fix: relay page data reservation FAILED\n");
        return false;
    }
    std::memset(flags, 0, 16);
    g_flags = flags;  // live before any gate can reach its stub

    bool ok = true;
    uint32_t n = 0;
    for (int b = 0; b < kGateBits; ++b) {
        if (!(mask & (1u << b))) continue;
        const Gate& g = kGates[b];
        const size_t len = stub_len(b);
        const uintptr_t stub = stubgen::reserve_code(len);
        uint8_t code[64];
        if (!stub || build_stub(b, reinterpret_cast<uintptr_t>(&flags[b]),
                                stub, code) != len) {
            log_line("[eu4cjk] wrap-fix %s: stub build FAILED\n", g.name);
            ok = false;
            continue;
        }
        std::memcpy(reinterpret_cast<void*>(stub), code, len); // page is RWX
        if (!stubgen::install_jmp(g.region, g.orig, g.len, stub)) {
            log_line("[eu4cjk] wrap-fix %s: install_jmp FAILED @0x%lx\n", g.name,
                     static_cast<unsigned long>(g.region));
            ok = false;
            continue;
        }
        ++n;
        log_line("[eu4cjk] wrap-fix: %s gate 0x%lx -> stub 0x%lx (%zu B)\n",
                 g.name, static_cast<unsigned long>(g.region),
                 static_cast<unsigned long>(stub), len);
    }
    log_line("[eu4cjk] wrap-fix: mask=0x%02X installed=%u/%d flags=%p\n",
             static_cast<unsigned>(mask), static_cast<unsigned>(n), kGateBits,
             static_cast<void*>(flags));
    return ok;
}

} // namespace eu4cjk::wrapfix
