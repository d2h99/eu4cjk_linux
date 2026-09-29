#include "selftest_h.h"

// Generic stubgen emission tests (frame slots, replay, rel32 guards).

namespace eu4cjk_test {

void run_stub_tests()
{
    std::memset(g_font, 0, sizeof(g_font));
    int marker_a = 0, marker_b = 0;
    *reinterpret_cast<void**>(g_font + 0x100 + 0x41 * 8) = &marker_a; // 'A'
    *reinterpret_cast<void**>(g_font + 0x100 + 0x42 * 8) = &marker_b; // 'B'
    const char str[] = "A";

    // --- site A shape: saves rax, replay reads font from [rsp+0x68] into rcx
    eu4cjk::stubgen::StubSpec spec;
    spec.saves[0].reg = eu4cjk::stubgen::Reg::Rax;
    spec.n_saves = 1;
    spec.args[0].kind = eu4cjk::stubgen::StubSpec::Arg::Imm32;
    spec.args[0].imm = 77;
    spec.args[1].kind = eu4cjk::stubgen::StubSpec::Arg::RegSrc;
    spec.args[1].src = eu4cjk::stubgen::Reg::Rax;
    spec.fn = reinterpret_cast<uintptr_t>(&test_helper);
    spec.replay = kOrigA;
    spec.replay_len = sizeof(kOrigA);
    spec.resume = reinterpret_cast<uintptr_t>(&ret_gadget);
    uintptr_t stub_a = eu4cjk::stubgen::emit(spec);
    check(stub_a != 0, "stubgen: emit site-A stub");

    register uint64_t r15_var asm("r15");
    r15_var = 0;
    uint64_t out_r15 = 0;
    // NOTE: results are stored to memory *inside* the asm block. Explicit
    // register variables are only guaranteed at the asm statement itself;
    // a later plain read of an rax-bound variable would race with any
    // intervening call clobbering rax (found the hard way: printf returned
    // 42 into eax between the asm and the readout).
    asm volatile(
        "sub $0x100, %%rsp\n\t"
        "movq %[fontp], 0x60(%%rsp)\n\t"   // stub entry rsp = here - 8 (call ret addr)
        "movq %[strp], %%rax\n\t"
        "call *%[stub]\n\t"
        "add $0x100, %%rsp\n\t"
        "movq %%r15, %[out]"
        : [out] "=m"(out_r15), "=r"(r15_var)
        : [strp] "r"(str), [fontp] "r"(g_font), [stub] "r"(stub_a)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
    check(g_stub_hits == 1 && g_stub_tag == 77 && g_stub_byte == 'A',
          "stubgen: site-A stub called helper with marshalled args");
    check(out_r15 == reinterpret_cast<uint64_t>(&marker_a),
          "stubgen: site-A replay loads glyph slot into r15");

    // --- site B shape: replay reads font from r12, writes result into rax
    spec.args[0].imm = 88;
    spec.replay = kOrigB;
    spec.replay_len = sizeof(kOrigB);
    uintptr_t stub_b = eu4cjk::stubgen::emit(spec);
    check(stub_b != 0, "stubgen: emit site-B stub");

    register uint64_t r12_var asm("r12");
    r12_var = reinterpret_cast<uint64_t>(g_font);
    const char strb[] = "B";
    uint64_t out_rax = 0;
    asm volatile(
        "sub $0x100, %%rsp\n\t"
        "movq %[strp], %%rax\n\t"
        "call *%[stub]\n\t"
        "add $0x100, %%rsp\n\t"
        "movq %%rax, %[out]"
        : [out] "=m"(out_rax), "+r"(r12_var)
        : [strp] "r"(strb), [stub] "r"(stub_b)
        : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
    check(g_stub_hits == 2 && g_stub_tag == 88 && g_stub_byte == 'B',
          "stubgen: site-B stub called helper");
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "stubgen: site-B replay loads rax (got 0x%llX want %p)",
                      static_cast<unsigned long long>(out_rax),
                      static_cast<void*>(&marker_b));
        check(out_rax == reinterpret_cast<uint64_t>(&marker_b), buf);
    }

    // --- install_jmp: verify + patch + pad (page near the stub page: rel32)
    uint8_t* site = static_cast<uint8_t*>(mmap(reinterpret_cast<void*>(0x21000000), 0x1000,
        PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
    check(site != MAP_FAILED, "stubgen: test page mapped");
    if (site != MAP_FAILED) {
        std::memcpy(site, kOrigA, sizeof(kOrigA));
        check(eu4cjk::stubgen::install_jmp(reinterpret_cast<uintptr_t>(site), kOrigA,
                                   sizeof(kOrigA), stub_a),
              "stubgen: install_jmp accepts matching bytes");
        bool shape = site[0] == 0xE9;
        for (size_t i = 5; i < sizeof(kOrigA); ++i) shape = shape && site[i] == 0x90;
        check(shape, "stubgen: install_jmp writes E9 + NOP pad");
        int32_t rel;
        std::memcpy(&rel, site + 1, 4);
        check(reinterpret_cast<uintptr_t>(site) + 5 + rel == stub_a,
               "stubgen: E9 rel32 lands on stub");
        site[2] ^= 0xFF;
        check(!eu4cjk::stubgen::install_jmp(reinterpret_cast<uintptr_t>(site), kOrigA,
                                    sizeof(kOrigA), stub_a),
               "stubgen: install_jmp rejects tampered bytes");
    }
}

} // namespace eu4cjk_test
