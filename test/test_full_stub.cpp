#include "selftest_h.h"

// Full-logic fetch stubs against the real site byte sequences.

namespace eu4cjk_test {

void run_full_stub_tests()
{
    std::memset(g_font, 0, sizeof(g_font));
    std::memset(g_test_ext, 0, sizeof(g_test_ext));
    int marker_a = 0, marker_ext = 0;
    *reinterpret_cast<void**>(g_font + 0x100 + 0x41 * 8) = &marker_a; // 'A'
    g_test_ext[0x4E2D] = &marker_ext;

    // --- emit site A stub (mirrors render.cpp install_fetch(site_a=true)).
    // Stub addresses live in globals: GCC stages asm register inputs through
    // stack slots that can overlap live locals (the M3-phase-2 lesson; it
    // ate stub_a/stub_b here, sending a driver into the wrong stub).
    eu4cjk::stubgen::StubSpec a;
    a.saves[0].reg = eu4cjk::stubgen::Reg::Rax;
    a.saves[0].restore = false;
    a.n_saves = 1;
    a.args[0].kind = eu4cjk::stubgen::StubSpec::Arg::RbpSlot;
    a.args[0].rbp_off = 0x70;
    a.args[1].kind = eu4cjk::stubgen::StubSpec::Arg::RegSrc;
    a.args[1].src = eu4cjk::stubgen::Reg::Rax;
    a.args[2].kind = eu4cjk::stubgen::StubSpec::Arg::FrameSlotLea;
    a.args[2].frame_off = 0x18;
    a.args[3].kind = eu4cjk::stubgen::StubSpec::Arg::RbpLea;
    a.args[3].rbp_off = 0x40;
    a.fn = reinterpret_cast<uintptr_t>(&test_fetch);
    a.has_result = true;
    a.result_reg = eu4cjk::stubgen::Reg::R15;
    static const uint8_t tailA[] = {
        0x48, 0x8B, 0x4D, 0x70,
        0x48, 0x8B, 0x44, 0x24, 0x00,
        0x0F, 0xB6, 0x00,
    };
    a.tail = tailA;
    a.tail_len = sizeof(tailA);
    a.has_advance = true;
    a.advance_frame_off = 0x18;
    a.advance_reg = eu4cjk::stubgen::Reg::R14;
    a.resume = reinterpret_cast<uintptr_t>(&ret_gadget);
    g_stub_a = eu4cjk::stubgen::emit(a);
    check(g_stub_a != 0, "fullstub: emit site-A");

    // --- emit site B stub (mirrors render.cpp install_fetch(site_a=false))
    eu4cjk::stubgen::StubSpec b;
    b.args[0].kind = eu4cjk::stubgen::StubSpec::Arg::RegSrc;
    b.args[0].src = eu4cjk::stubgen::Reg::R12;
    b.args[1].kind = eu4cjk::stubgen::StubSpec::Arg::RegSrc;
    b.args[1].src = eu4cjk::stubgen::Reg::Rax;
    b.args[2].kind = eu4cjk::stubgen::StubSpec::Arg::FrameSlotLea;
    b.args[2].frame_off = 0x18;
    b.args[3].kind = eu4cjk::stubgen::StubSpec::Arg::Imm32; // acc = nullptr
    b.args[3].imm = 0;
    b.fn = reinterpret_cast<uintptr_t>(&test_fetch);
    b.has_advance = true;
    b.advance_frame_off = 0x18;
    b.advance_reg = eu4cjk::stubgen::Reg::Rbx;
    b.resume = reinterpret_cast<uintptr_t>(&ret_gadget);
    g_stub_b = eu4cjk::stubgen::emit(b);
    check(g_stub_b != 0, "fullstub: emit site-B");

    if (!g_stub_a || !g_stub_b) return;

    const char esc[] = "\x10\x2D\x4E";
    const char plain[] = "A";

    // --- execute site A driver: escape case
    register uint64_t r14_var asm("r14");
    register uint64_t r12_var asm("r12");
    r14_var = 5;
    r12_var = 0x5805CAFE; // fake empty-string sentinel
    asm volatile(
        "sub $0x100, %%rsp\n\t"
        "movq %[fontp], 0x60(%%rsp)\n\t"
        "movq %[strp], %%rax\n\t"
        "call *%[stub]\n\t"
        "movq %%rsp, %[osp]\n\t"
        "add $0x100, %%rsp\n\t"
        "movq %%r15, %[o15]\n\t"
        "movq %%rcx, %[ocx]\n\t"
        "movq %%rax, %[oax]"
        : [o15] "=m"(g_out15), [ocx] "=m"(g_outcx), [oax] "=m"(g_outax), [osp] "=m"(g_out_sp),
          "+r"(r14_var), "+r"(r12_var)
        : [strp] "r"(esc), [fontp] "r"(g_font), [stub] "r"(g_stub_a)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r15", "memory", "cc");
    check(g_out15 == reinterpret_cast<uint64_t>(&marker_ext),
          "fullstub A(escape): external glyph into r15");
    check(r14_var == 7, "fullstub A(escape): index advanced +2");
    check(g_outcx == reinterpret_cast<uint64_t>(g_font), "fullstub A(escape): rcx = font");
    check(g_outax == 0x10, "fullstub A(escape): rax = prefix byte");
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "fullstub A(escape): acc = rsp+0x30 (got %p, rsp 0x%llX)",
                      g_got_acc, static_cast<unsigned long long>(g_out_sp));
        check(g_got_acc == reinterpret_cast<void*>(g_out_sp + 0x30), buf);
    }

    // --- execute site A driver: plain case
    r14_var = 5;
    asm volatile(
        "sub $0x100, %%rsp\n\t"
        "movq %[fontp], 0x60(%%rsp)\n\t"
        "movq %[strp], %%rax\n\t"
        "call *%[stub]\n\t"
        "add $0x100, %%rsp\n\t"
        "movq %%r15, %[o15]\n\t"
        "movq %%rcx, %[ocx]\n\t"
        "movq %%rax, %[oax]"
        : [o15] "=m"(g_out15), [ocx] "=m"(g_outcx), [oax] "=m"(g_outax), "+r"(r14_var)
        : [strp] "r"(plain), [fontp] "r"(g_font), [stub] "r"(g_stub_a)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r15", "memory", "cc");
    check(g_out15 == reinterpret_cast<uint64_t>(&marker_a),
          "fullstub A(plain): inline glyph into r15");
    check(r14_var == 5, "fullstub A(plain): index unchanged");
    check(g_outax == 'A', "fullstub A(plain): rax = byte");

    // --- execute site B driver: escape case
    register uint64_t rbx_var asm("rbx");
    r12_var = reinterpret_cast<uint64_t>(g_font);
    rbx_var = 3;
    asm volatile(
        "sub $0x100, %%rsp\n\t"
        "movq %[strp], %%rax\n\t"
        "call *%[stub]\n\t"
        "add $0x100, %%rsp\n\t"
        "movq %%rax, %[oax]\n\t"
        "movq %%rbx, %[obx]"
        : [oax] "=m"(g_outb), [obx] "=m"(g_outbx), "+r"(r12_var), "+r"(rbx_var)
        : [strp] "r"(esc), [stub] "r"(g_stub_b)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
    check(g_outbx == 5, "fullstub B(escape): index advanced +2");
    {
        // single authoritative check; the embedded values keep the read fresh
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "fullstub B(escape): external glyph into rax"
                      " (got 0x%llX want %p)", static_cast<unsigned long long>(g_outb),
                      static_cast<void*>(&marker_ext));
        check(g_outb == reinterpret_cast<uint64_t>(&marker_ext), buf);
    }

    // --- execute site B driver: plain case
    rbx_var = 3;
    asm volatile(
        "sub $0x100, %%rsp\n\t"
        "movq %[strp], %%rax\n\t"
        "call *%[stub]\n\t"
        "add $0x100, %%rsp\n\t"
        "movq %%rax, %[oax]\n\t"
        "movq %%rbx, %[obx]"
        : [oax] "=m"(g_outb), [obx] "=m"(g_outbx), "+r"(r12_var), "+r"(rbx_var)
        : [strp] "r"(plain), [stub] "r"(g_stub_b)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "rbp", "memory", "cc");
    check(g_outb == reinterpret_cast<uint64_t>(&marker_a),
          "fullstub B(plain): inline glyph into rax");
    check(g_outbx == 3, "fullstub B(plain): index unchanged");

    // --- emit R2T-1 stub (mirrors render.cpp install_fetch_r2t wrap_loop):
    // font via [entry_rsp+0x38], acc = entry_rsp+0x58, node into rbp via
    // post_tail (frame anchor forbids touching rbp inside the frame).
    eu4cjk::stubgen::StubSpec c;
    c.saves[0].reg = eu4cjk::stubgen::Reg::Rax;
    c.saves[0].restore = false;
    c.n_saves = 1;
    c.args[0].kind = eu4cjk::stubgen::StubSpec::Arg::RbpSlot;
    c.args[0].rbp_off = 0x40;
    c.args[1].kind = eu4cjk::stubgen::StubSpec::Arg::RegSrc;
    c.args[1].src = eu4cjk::stubgen::Reg::Rax;
    c.args[2].kind = eu4cjk::stubgen::StubSpec::Arg::FrameSlotLea;
    c.args[2].frame_off = 0x18;
    c.args[3].kind = eu4cjk::stubgen::StubSpec::Arg::RbpLea;
    c.args[3].rbp_off = 0x60;
    c.args[4].kind = eu4cjk::stubgen::StubSpec::Arg::Imm32;
    c.args[4].imm = 2;
    c.fn = reinterpret_cast<uintptr_t>(&test_fetch);
    c.has_result = true;
    c.result_reg = eu4cjk::stubgen::Reg::R11;
    static const uint8_t tailC[] = {
        0x48, 0x8B, 0x4D, 0x40,
        0x48, 0x8B, 0x04, 0x24,
        0x0F, 0xB6, 0x00,
    };
    c.tail = tailC;
    c.tail_len = sizeof(tailC);
    static const uint8_t postC[] = { 0x4C, 0x89, 0xDD }; // mov rbp, r11
    c.post_tail = postC;
    c.post_tail_len = sizeof(postC);
    c.has_advance = true;
    c.advance_frame_off = 0x18;
    c.advance_reg = eu4cjk::stubgen::Reg::R14;
    c.resume = reinterpret_cast<uintptr_t>(&ret_gadget);
    g_stub_c = eu4cjk::stubgen::emit(c);
    check(g_stub_c != 0, "fullstub: emit R2T-1");
    if (g_stub_c) {
        // driver frame: font at [rsp+0x30] (= stub's [entry_rsp+0x38]),
        // acc expected at rsp+0x50 (= entry_rsp+0x58)
        r14_var = 9;
        asm volatile(
            "sub $0x100, %%rsp\n\t"
            "movq %[fontp], 0x30(%%rsp)\n\t"
            "movq %[strp], %%rax\n\t"
            "call *%[stub]\n\t"
            "movq %%rsp, %[osp]\n\t"
            "add $0x100, %%rsp\n\t"
            "movq %%rbp, %[obp]\n\t"
            "movq %%rcx, %[ocx]\n\t"
            "movq %%rax, %[oax]"
            : [obp] "=m"(g_outbp), [ocx] "=m"(g_outcx2), [oax] "=m"(g_outax2),
              [osp] "=m"(g_out_sp2), "+r"(r14_var)
            : [strp] "r"(esc), [fontp] "r"(g_font), [stub] "r"(g_stub_c)
            : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
              "rbp", "memory", "cc");
        check(g_outbp == reinterpret_cast<uint64_t>(&marker_ext),
              "fullstub R2T1(escape): node into rbp via post_tail");
        check(r14_var == 11, "fullstub R2T1(escape): index advanced +2");
        check(g_outcx2 == reinterpret_cast<uint64_t>(g_font),
              "fullstub R2T1(escape): rcx = font");
        check(g_outax2 == 0x10, "fullstub R2T1(escape): rax = prefix byte");
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "fullstub R2T1(escape): acc = rsp+0x50 (got %p,"
                          " rsp 0x%llX)", g_got_acc,
                          static_cast<unsigned long long>(g_out_sp2));
            check(g_got_acc == reinterpret_cast<void*>(g_out_sp2 + 0x50), buf);
        }

        r14_var = 9;
        asm volatile(
            "sub $0x100, %%rsp\n\t"
            "movq %[fontp], 0x30(%%rsp)\n\t"
            "movq %[strp], %%rax\n\t"
            "call *%[stub]\n\t"
            "add $0x100, %%rsp\n\t"
            "movq %%rbp, %[obp]\n\t"
            "movq %%rcx, %[ocx]\n\t"
            "movq %%rax, %[oax]"
            : [obp] "=m"(g_outbp), [ocx] "=m"(g_outcx2), [oax] "=m"(g_outax2),
              "+r"(r14_var)
            : [strp] "r"(plain), [fontp] "r"(g_font), [stub] "r"(g_stub_c)
            : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
              "rbp", "memory", "cc");
        check(g_outbp == reinterpret_cast<uint64_t>(&marker_a),
              "fullstub R2T1(plain): inline glyph into rbp");
        check(r14_var == 9, "fullstub R2T1(plain): index unchanged");
        check(g_outax2 == 'A', "fullstub R2T1(plain): rax = byte");
    }
}

} // namespace eu4cjk_test
