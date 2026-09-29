#include "selftest_h.h"

#include <cmath>
#include <cstring>

// CurveText stub tests: ct-tdiv (M4-P2) + ct-single (P2e).

namespace eu4cjk_test {

// CurveText t-numerator stub (M4-P2): execute the real stub bytes against a
// fake frame; xmm6 must equal (float)(r14d - 2) when r14d sits on the last
// payload byte of an escape, else (float)r14d.
void run_ct_tdiv_tests()
{
    uint8_t bytes[64];
    const size_t len = eu4cjk::render::build_ct_tdiv_stub(
        bytes, sizeof(bytes), reinterpret_cast<uintptr_t>(&ret_gadget));
    check(len > 0, "ct-tdiv: builder length");

    const uintptr_t stub = eu4cjk::stubgen::emit_raw(bytes, len);
    check(stub != 0, "ct-tdiv: emit_raw");
    if (!stub) return;

    // "康宽␣" = escape(3B) + escape(3B) + space; CString* -> {buf, len}
    static const char kStr[] = { 0x10, (char)0xB7, (char)0x5E,
                                 0x12, 0x22, 0x23, 0x20, 0x00 };
    static void* cstr[2] = { const_cast<char*>(kStr), nullptr };
    static uint8_t frame[0x400];
    const uintptr_t rbp = reinterpret_cast<uintptr_t>(frame) + 0x200;
    *reinterpret_cast<void**>(rbp - 0x148) = cstr;   // CurveText: [rbp-0x148] = CString*

    auto run = [&](uint64_t r14_in) -> float {
        register uint64_t r14_var asm("r14") = r14_in;
        register uint64_t rbp_var asm("rbp") = rbp;
        uint32_t out_bits = 0;
        asm volatile(
            "call *%[s]\n\t"
            "movd %%xmm6, %[o]"
            : [o] "=m"(out_bits), "+r"(r14_var), "+r"(rbp_var)
            : [s] "r"(stub)
            : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
              "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
              "memory", "cc");
        float f;
        std::memcpy(&f, &out_bits, 4);
        return f;
    };
    check(run(2) == 0.0f, "ct-tdiv: escape1 last payload byte -> 0");
    check(run(5) == 3.0f, "ct-tdiv: escape2 last payload byte -> 3");
    check(run(6) == 6.0f, "ct-tdiv: space after escapes -> unchanged");
    check(run(1) == 1.0f, "ct-tdiv: r14d<2 -> unchanged");
}

// CurveText single-glyph sampler stub (M4-P2 detection, M6-2 Win-parity
// apply): a name is a TRUE single-glyph label iff its length-bounded
// string contains exactly ONE escape marker (0x10..0x13; escapes are
// uniformly 3 bytes). The stub scans data[3..len): any second escape ->
// replay untouched; scan exhausted -> zero the lastMid slots [-0xd8] and
// [-0x68] ([-0x70].high), reproducing MSVC's pre-loop zero-init of the
// mid slots (eu4.exe 1.37.5 @ 0x140fd3976-89) which the clang build
// lacks, then replays mov -0xd8(%rbp),%r14. The engine's own rotation
// math then yields cos/sin = normalize(avg - 0), the slight per-label
// tilt seen on Windows. Tail junk past the length (heap noise) and
// non-escape tail bytes (transform leftovers like '1' 'r') must NOT
// block the fix; spaced/unspaced/0x12 multi-glyph and ASCII strings must
// stay untouched.
void run_ct_single_tests()
{
    uint8_t bytes[384];
    const size_t len = eu4cjk::render::build_ct_single_stub(
        bytes, sizeof(bytes), reinterpret_cast<uintptr_t>(&ret_gadget));
    check(len > 0, "ct-single: builder length");

    const uintptr_t stub = eu4cjk::stubgen::emit_raw(bytes, len);
    check(stub != 0, "ct-single: emit_raw");
    if (!stub) return;

    static uint8_t frame[0x400];
    static uint8_t cstr_obj[16];
    static uint8_t str_bytes[32];
    const uintptr_t rbp = reinterpret_cast<uintptr_t>(frame) + 0x300;
    *reinterpret_cast<uintptr_t*>(frame + 0x300 - 0x148) =
        reinterpret_cast<uintptr_t>(cstr_obj);
    *reinterpret_cast<uintptr_t*>(cstr_obj) =
        reinterpret_cast<uintptr_t>(str_bytes);
    auto slotd = [&](int off) -> double* {
        return reinterpret_cast<double*>(rbp + off);
    };

    const double avgx = 3000.0, avgy = 1600.0;
    // n = explicit CString length (field at +8); buffer is zero-filled
    // past it to prove the scan never reads beyond the length.
    auto run = [&](const uint8_t* str, int n) -> uint64_t {
        std::memset(str_bytes, 0, sizeof(str_bytes));
        std::memcpy(str_bytes, str, static_cast<size_t>(n));
        *reinterpret_cast<int*>(cstr_obj + 8) = n;
        *slotd(-0xe0) = 111.0;
        *slotd(-0xd8) = 222.0;
        *slotd(-0x70) = 333.0;
        *slotd(-0x68) = 444.0;
        register uint64_t rbp_var asm("rbp") = rbp;
        uint64_t out_r14 = 0;
        asm volatile(
            "movsd %[ax], %%xmm2\n\t"
            "movhpd %[ay], %%xmm2\n\t"
            "call *%[s]\n\t"
            "movq %%r14, %[o]"
            : [o] "=m"(out_r14), "+r"(rbp_var)
            : [ax] "m"(avgx), [ay] "m"(avgy), [s] "r"(stub)
            : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
              "r14", "xmm0", "xmm1", "xmm2", "memory", "cc");
        return out_r14;
    };

    auto check_untouched = [&](const char* tag) {
        check(*slotd(-0xe0) == 111.0 && *slotd(-0xd8) == 222.0 &&
              *slotd(-0x70) == 333.0 && *slotd(-0x68) == 444.0, tag);
    };
    auto check_applied = [&]() {
        const double len = std::sqrt(avgx * avgx + avgy * avgy);
        const double ux = avgx / len, uy = avgy / len, K = 4.9;
        check(*slotd(-0xd8) == avgx - ux * K, "ct-single: [-0xd8] = avg.x - ux*K");
        check(*slotd(-0x68) == avgy - uy * K, "ct-single: [-0x68] = avg.y - uy*K");
        check(*slotd(-0x70) == avgx + ux * K, "ct-single: [-0x70].low = avg.x + ux*K");
        check(*slotd(-0xe0) == avgy + uy * K, "ct-single: [-0xe0] = avg.y + uy*K");
    };
    double d;

    // 1. 坎-style: escape + 3 letter-spaces
    const uint8_t kan[] = {0x10, 0x4E, 0x57, 0x20, 0x20, 0x20};
    uint64_t r14 = run(kan, 6);
    check_applied();
    std::memcpy(&d, &r14, 8);
    {
        const double len = std::sqrt(avgx * avgx + avgy * avgy);
        check(d == avgx - (avgx / len) * 4.9, "ct-single: 坎 replay loads r14 = new [-0xd8]");
    }

    // 2. bare escape only (len 3)
    const uint8_t bare[] = {0x10, 0x4E, 0x57};
    run(bare, 3);
    check_applied();

    // 3. 劳-style: len 4 single + heap noise past length (0x31 0x72 0x10 ..)
    const uint8_t noise[] = {0x10, 0xB3, 0x52, 0x20, 0x31, 0x72, 0x10, 0x93};
    run(noise, 4);
    check_applied();

    // 4. transform-leftover tail inside length (renders no vertices)
    const uint8_t junk[] = {0x10, 0xB3, 0x52, 0x20, 0x31, 0x72};
    run(junk, 6);
    check_applied();

    // 5. 0x11-led single
    const uint8_t e11[] = {0x11, 0x61, 0x84, 0x20, 0x20};
    run(e11, 5);
    check_applied();

    // 6. 奥斯曼-style: escape + 1 space + second escape
    const uint8_t ottoman[] = {0x10, 0x65, 0x59, 0x20, 0x10, 0x65, 0xAF, 0x20};
    run(ottoman, 8);
    check_untouched("ct-single: spaced multi-glyph (奥斯曼) untouched");

    // 7. 西藏-style: escape + 2 spaces + second escape
    const uint8_t xizang[] = {0x10, 0x7F, 0x89, 0x20, 0x20, 0x10, 0x27, 0x59};
    run(xizang, 8);
    check_untouched("ct-single: spaced multi-glyph (西藏) untouched");

    // 8. 大清-style: unspaced multi-glyph
    const uint8_t daqing[] = {0x10, 0x27, 0x59, 0x10, 0x71, 0x51, 0x20};
    run(daqing, 7);
    check_untouched("ct-single: unspaced multi-glyph (大清) untouched");

    // 9. 0x12 second escape (隆德-style capture)
    const uint8_t e12[] = {0x10, 0x86, 0x96, 0x20, 0x12, 0xB7, 0x56, 0x20};
    run(e12, 8);
    check_untouched("ct-single: 0x12 second escape untouched");

    // 10. ASCII
    const uint8_t ascii[] = {'T', 'e', 's', 't'};
    r14 = run(ascii, 4);
    check_untouched("ct-single: ascii untouched");
    std::memcpy(&d, &r14, 8);
    check(d == 222.0, "ct-single: replay loads original [-0xd8] when skipped");
}

} // namespace eu4cjk_test
