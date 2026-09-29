#include "selftest_h.h"

// Measurement-family stub tests (M-001..M-005 register/save contracts).

namespace eu4cjk_test {

void run_measure_stub_tests()
{
    std::memset(g_font, 0, sizeof(g_font));
    std::memset(g_test_ext, 0, sizeof(g_test_ext));
    int marker_a = 0, marker_ext = 0;
    *reinterpret_cast<void**>(g_font + 0x100 + 0x41 * 8) = &marker_a; // 'A'
    g_test_ext[0x4E2D] = &marker_ext;
    const char esc[] = "\x10\x2D\x4E";
    const char plain[] = "A";

    // --- GW (emit first; drivers run after all emits)
    eu4cjk::stubgen::StubSpec w;
    w.saves[0].reg = eu4cjk::stubgen::Reg::Rdi; // font restore (resume path
    w.saves[0].restore = true;                   // uses rdi: mulss [rdi+0x940])
    w.n_saves = 1;
    w.args[0].kind = eu4cjk::stubgen::StubSpec::Arg::RegSrc;
    w.args[0].src = eu4cjk::stubgen::Reg::Rdi;
    w.args[1].kind = eu4cjk::stubgen::StubSpec::Arg::IdxSextSum;
    w.args[1].idx = eu4cjk::stubgen::Reg::R15;
    w.args[1].base = eu4cjk::stubgen::Reg::Rbx;
    w.args[1].has_base = true;
    w.args[2].kind = eu4cjk::stubgen::StubSpec::Arg::FrameSlotLea;
    w.args[2].frame_off = 0x18;
    w.args[3].kind = eu4cjk::stubgen::StubSpec::Arg::Imm32;
    w.args[3].imm = 0;
    w.args[4].kind = eu4cjk::stubgen::StubSpec::Arg::Imm32;
    w.args[4].imm = 10;
    w.fn = reinterpret_cast<uintptr_t>(&test_fetch);
    w.has_result = true;
    w.result_reg = eu4cjk::stubgen::Reg::R11;
    static const uint8_t tailW[] = {
        0x42, 0x0F, 0xB6, 0x34, 0x3B,         // movzx esi,BYTE PTR [rbx+r15*1]
    };
    w.tail = tailW;
    w.tail_len = sizeof(tailW);
    static const uint8_t postW[] = { 0x4C, 0x89, 0xDD };
    w.post_tail = postW;
    w.post_tail_len = sizeof(postW);
    w.has_advance = true;
    w.advance_frame_off = 0x18;
    w.advance_reg = eu4cjk::stubgen::Reg::R14;
    w.resume = reinterpret_cast<uintptr_t>(&ret_gadget);
    g_stub_gw = eu4cjk::stubgen::emit(w);
    check(g_stub_gw != 0, "measure GW: emit");

    // --- GH
    eu4cjk::stubgen::StubSpec h;
    h.args[0].kind = eu4cjk::stubgen::StubSpec::Arg::RegSrc;
    h.args[0].src = eu4cjk::stubgen::Reg::Rbx;
    h.args[1].kind = eu4cjk::stubgen::StubSpec::Arg::RegSrc;
    h.args[1].src = eu4cjk::stubgen::Reg::Rax;
    h.args[2].kind = eu4cjk::stubgen::StubSpec::Arg::FrameSlotLea;
    h.args[2].frame_off = 0x18;
    h.args[3].kind = eu4cjk::stubgen::StubSpec::Arg::Imm32;
    h.args[3].imm = 0;
    h.args[4].kind = eu4cjk::stubgen::StubSpec::Arg::Imm32;
    h.args[4].imm = 11;
    h.fn = reinterpret_cast<uintptr_t>(&test_fetch);
    h.has_advance_mem = true;
    h.advance_frame_off = 0x18;
    h.advance_mem_rbp_off = 0;                // the stub's pushed engine rbp
    h.resume = reinterpret_cast<uintptr_t>(&ret_gadget);
    g_stub_gh = eu4cjk::stubgen::emit(h);
    check(g_stub_gh != 0, "measure GH: emit");

    // --- GR
    eu4cjk::stubgen::StubSpec r;
    r.args[0].kind = eu4cjk::stubgen::StubSpec::Arg::RegSrc;
    r.args[0].src = eu4cjk::stubgen::Reg::R13;
    r.args[1].kind = eu4cjk::stubgen::StubSpec::Arg::RegSrc;
    r.args[1].src = eu4cjk::stubgen::Reg::Rax;
    r.args[2].kind = eu4cjk::stubgen::StubSpec::Arg::FrameSlotLea;
    r.args[2].frame_off = 0x18;
    r.args[3].kind = eu4cjk::stubgen::StubSpec::Arg::Imm32;
    r.args[3].imm = 0;
    r.args[4].kind = eu4cjk::stubgen::StubSpec::Arg::Imm32;
    r.args[4].imm = 13;
    r.fn = reinterpret_cast<uintptr_t>(&test_fetch);
    r.has_result = true;
    r.result_reg = eu4cjk::stubgen::Reg::R11;
    static const uint8_t postR[] = { 0x4C, 0x89, 0xDD };
    r.post_tail = postR;
    r.post_tail_len = sizeof(postR);
    r.has_advance_mem = true;
    r.advance_frame_off = 0x18;
    // NOTE: models a dword engine slot at [entry_rsp+0xC] (rbp_off 0x14).
    // The game site keeps its index at [entry_rsp+0x4] (rbp_off 0xC) - fine
    // there because install_jmp enters via JMP. A call-based TEST driver
    // cannot model [entry_rsp+0..7]: the pushed return address occupies
    // exactly those 8 bytes and the +2 landed in its high dword (found the
    // hard way: ret jumped to 0x2_00xxxxxx).
    r.advance_mem_rbp_off = 0x14;
    r.resume = reinterpret_cast<uintptr_t>(&ret_gadget);
    g_stub_gr = eu4cjk::stubgen::emit(r);
    check(g_stub_gr != 0, "measure GR: emit");

    if (!g_stub_gw || !g_stub_gh || !g_stub_gr) return;

    register uint64_t r14_var asm("r14");
    register uint64_t rbx_var asm("rbx");
    register uint64_t r13_var asm("r13");
    register uint64_t r15_var asm("r15");

    // --- GW escape: rbp = ext node, rsi = 0x10, r14 +2, rdi = font restored
    r14_var = 5;
    r15_var = 0;
    rbx_var = reinterpret_cast<uint64_t>(esc);
    asm volatile(
        "movq %[fontp], %%rdi\n\t"
        "call *%[stub]\n\t"
        "movq %%rbp, %[obp]\n\t"
        "movq %%rsi, %[osi]\n\t"
        "movq %%rdi, %[odi]"
        : [obp] "=m"(g_mgw_rbp), [osi] "=m"(g_mgw_rsi), [odi] "=m"(g_mgw_rdi),
          "+r"(r14_var), "+r"(rbx_var), "+r"(r15_var)
        : [fontp] "r"(g_font), [stub] "r"(g_stub_gw)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "rbp", "memory", "cc");
    check(g_mgw_rbp == reinterpret_cast<uint64_t>(&marker_ext),
          "measure GW(escape): node into rbp via post_tail");
    check(g_mgw_rsi == 0x10, "measure GW(escape): rsi = prefix byte");
    check(r14_var == 7, "measure GW(escape): index r14d advanced +2");
    check(g_mgw_rdi == reinterpret_cast<uint64_t>(g_font),
          "measure GW(escape): rdi = font restored (resume path needs it)");

    // --- GW plain: rbp = inline node, rsi = 'A', r14 unchanged
    r14_var = 5;
    r15_var = 0; // rebind: GCC staged the escape run's [stub] input in r15
    rbx_var = reinterpret_cast<uint64_t>(plain);
    asm volatile(
        "movq %[fontp], %%rdi\n\t"
        "call *%[stub]\n\t"
        "movq %%rbp, %[obp]\n\t"
        "movq %%rsi, %[osi]"
        : [obp] "=m"(g_mgw_rbp), [osi] "=m"(g_mgw_rsi), "+r"(r14_var),
          "+r"(rbx_var), "+r"(r15_var)
        : [fontp] "r"(g_font), [stub] "r"(g_stub_gw)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "rbp", "memory", "cc");
    check(g_mgw_rbp == reinterpret_cast<uint64_t>(&marker_a),
          "measure GW(plain): inline node into rbp");
    check(g_mgw_rsi == 'A', "measure GW(plain): rsi = byte");
    check(r14_var == 5, "measure GW(plain): index unchanged");

    // --- GH escape: rax = ext node; engine rbp (index 9) +2 via pushed slot.
    // rbp is set/read inside the asm (GCC reserves it as frame pointer). The
    // sub $0x100 pad keeps the stub frame OUT of the red zone - GCC stages
    // asm inputs there, and the stub's push/anchor/+2 would corrupt them
    // (found the hard way: bogus check args and wild jumps afterwards).
    rbx_var = reinterpret_cast<uint64_t>(g_font);
    asm volatile(
        "sub $0x100, %%rsp\n\t"
        "movq $9, %%rbp\n\t"
        "movq %[strp], %%rax\n\t"
        "call *%[stub]\n\t"
        "add $0x100, %%rsp\n\t"
        "movq %%rax, %[oax]\n\t"
        "movq %%rbp, %[obp]"
        : [oax] "=m"(g_mgh_rax), [obp] "=m"(g_mgh_rbp), "+r"(rbx_var)
        : [strp] "r"(esc), [stub] "r"(g_stub_gh)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "rbp", "memory", "cc");
    check(g_mgh_rax == reinterpret_cast<uint64_t>(&marker_ext),
          "measure GH(escape): node stays in rax");
    check(g_mgh_rbp == 11, "measure GH(escape): engine rbp index advanced +2");

    // --- GH plain (rbx re-bound: GCC otherwise stages the string in rbx,
    // which the stub marshals as the font)
    rbx_var = reinterpret_cast<uint64_t>(g_font);
    asm volatile(
        "sub $0x100, %%rsp\n\t"
        "movq $9, %%rbp\n\t"
        "movq %[strp], %%rax\n\t"
        "call *%[stub]\n\t"
        "add $0x100, %%rsp\n\t"
        "movq %%rax, %[oax]\n\t"
        "movq %%rbp, %[obp]"
        : [oax] "=m"(g_mgh_rax), [obp] "=m"(g_mgh_rbp), "+r"(rbx_var)
        : [strp] "r"(plain), [stub] "r"(g_stub_gh)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "rbp", "memory", "cc");
    check(g_mgh_rax == reinterpret_cast<uint64_t>(&marker_a),
          "measure GH(plain): inline node into rax");
    check(g_mgh_rbp == 9, "measure GH(plain): engine rbp unchanged");

    // --- GR escape: engine dword [entry_rsp+0xC] = 21 -> 23; rbp = ext node.
    // (call pushes the return address at [entry_rsp..+7], so the modeled
    // slot lives at driver [rsp+4] = [entry_rsp+0xC])
    r13_var = reinterpret_cast<uint64_t>(g_font);
    asm volatile(
        "sub $0x100, %%rsp\n\t"
        "movl $21, 0x4(%%rsp)\n\t"
        "movq %[strp], %%rax\n\t"
        "call *%[stub]\n\t"
        "movl 0x4(%%rsp), %%eax\n\t"
        "movq %%rax, %[oslot]\n\t"
        "add $0x100, %%rsp\n\t"
        "movq %%rbp, %[obp]"
        : [oslot] "=m"(g_mgr_slot_after), [obp] "=m"(g_mgr_rbp), "+r"(r13_var)
        : [strp] "r"(esc), [stub] "r"(g_stub_gr)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "rbp", "memory", "cc");
    check(g_mgr_slot_after == 23,
          "measure GR(escape): engine frame index slot advanced +2");
    check(g_mgr_rbp == reinterpret_cast<uint64_t>(&marker_ext),
          "measure GR(escape): node into rbp via post_tail");

    // --- GR plain: slot unchanged, rbp = inline node
    r13_var = reinterpret_cast<uint64_t>(g_font); // rebind (GCC may stage
                                                  // [stub] in r13 otherwise)
    asm volatile(
        "sub $0x100, %%rsp\n\t"
        "movl $21, 0x4(%%rsp)\n\t"
        "movq %[strp], %%rax\n\t"
        "call *%[stub]\n\t"
        "movl 0x4(%%rsp), %%eax\n\t"
        "movq %%rax, %[oslot]\n\t"
        "add $0x100, %%rsp\n\t"
        "movq %%rbp, %[obp]"
        : [oslot] "=m"(g_mgr_slot_after), [obp] "=m"(g_mgr_rbp), "+r"(r13_var)
        : [strp] "r"(plain), [stub] "r"(g_stub_gr)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "rbp", "memory", "cc");
    check(g_mgr_slot_after == 21, "measure GR(plain): slot unchanged");
    check(g_mgr_rbp == reinterpret_cast<uint64_t>(&marker_a),
          "measure GR(plain): inline node into rbp");
}

} // namespace eu4cjk_test
