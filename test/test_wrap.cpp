#include "selftest_h.h"

// M4-P1d wrap-gate stub tests (CJK break-candidate bit).

namespace eu4cjk_test {

void run_wrap_stub_tests()
{
    using namespace eu4cjk::wrapfix;

    static uint8_t flags[16];
    std::memset(flags, 0, sizeof(flags));
    bind_flags_for_test(flags);

    // --- note_char site -> bit mapping
    note_char(0, true);
    note_char(2, true);
    note_char(10, true);
    note_char(6, true);
    note_char(4, true);
    note_char(1, true);   // no gate for site 1
    check(flags[0] && flags[1] && flags[2] && flags[3] && flags[4],
          "wrapfix: note_char maps sites 0/2/10/6/4 -> bits 0-4");
    note_char(0, false);
    check(flags[0] == 0, "wrapfix: note_char clears flag");
    std::memset(flags, 0, sizeof(flags));

    // --- reserve_data: 8-aligned, distinct
    {
        void* a = eu4cjk::stubgen::reserve_data(16);
        void* b = eu4cjk::stubgen::reserve_data(16);
        check(a && b && a != b
                  && (reinterpret_cast<uintptr_t>(a) & 7u) == 0,
              "wrapfix: stubgen reserve_data aligned + distinct");
    }

    // --- golden: bit0 (FillVB form) disp32/rel32 math
    {
        uint8_t code[64];
        const int64_t fs = 0x30000100, ff = 0x3000F000;
        size_t len = build_stub_ex(0, static_cast<uintptr_t>(ff),
                                   static_cast<uintptr_t>(fs),
                                   0x30000A00, 0x30000B00, code);
        check(len == stub_len(0), "wrapfix: bit0 stub length");
        int32_t v;
        std::memcpy(&v, code + 2, 4);
        check(fs + 7 + v == ff, "wrapfix: bit0 flag disp32 lands on flag byte");
        std::memcpy(&v, code + 9, 4);
        check(fs + 13 + v == 0x30000B00, "wrapfix: bit0 je -> untaken");
        std::memcpy(&v, code + 14, 4);
        check(fs + 18 + v == 0x30000A00, "wrapfix: bit0 jmp -> taken");
    }

    // --- emit all 5 stubs first (emit-then-run, see run_full_stub_tests)
    const uintptr_t taken = reinterpret_cast<uintptr_t>(&wrap_pad_taken);
    const uintptr_t untaken = reinterpret_cast<uintptr_t>(&wrap_pad_untaken);
    uintptr_t stubs[kGateBits] = {};
    bool emit_ok = true;
    for (int b = 0; b < kGateBits; ++b) {
        const size_t len = stub_len(b);
        stubs[b] = eu4cjk::stubgen::reserve_code(len);
        uint8_t code[64];
        if (!stubs[b] ||
            build_stub_ex(b, reinterpret_cast<uintptr_t>(&flags[b]), stubs[b],
                          taken, untaken, code) != len) {
            emit_ok = false;
            break;
        }
        std::memcpy(reinterpret_cast<void*>(stubs[b]), code, len);
    }
    check(emit_ok, "wrapfix: all 5 gate stubs emitted");
    if (!emit_ok) return;
    if (std::getenv("EU4CJK_SELFTEST_DIAG")) {
        for (int b = 0; b < kGateBits; ++b) {
            char hb[4 * 64];
            size_t hk = 0;
            const uint8_t* pm = reinterpret_cast<const uint8_t*>(stubs[b]);
            for (size_t i = 0; i < stub_len(b); ++i)
                hk += static_cast<size_t>(std::snprintf(hb + hk, sizeof(hb) - hk,
                                                        "%02x ", pm[i]));
            std::printf("diag: stub%d @%p flagslot=%p: %s\n", b,
                        reinterpret_cast<void*>(stubs[b]),
                        static_cast<void*>(&flags[b]), hb);
        }
    }

    alignas(16) uint16_t node_glyph[8] = {};
    node_glyph[3] = 12;                       // h != 0 (CJK node shape)
    alignas(16) uint16_t node_space[8] = {};  // h == 0 (space shape)

    // generic rbp-node driver (bits 1/2/3)
    // NOTE: %[stub] is moved into r11 (clobbered -> never an input register)
    // BEFORE rbp is touched: GCC may stage inputs through rbp itself (same
    // trap as the GR tests' r13 note).
    auto run_rbp = [&](uintptr_t stub, const uint16_t* node) {
        g_wrap_land = 0;
        asm volatile(
            "mov %[stub], %%r11\n\t"
            "push %%rbp\n\t"
            "mov %[node], %%rbp\n\t"
            "call *%%r11\n\t"
            "pop %%rbp"
            :
            : [node] "r"(node), [stub] "r"(stub)
            : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
              "memory", "cc");
    };

    // --- bit0 FillVB: entered on h!=0 only; flag decides
    flags[0] = 0; g_wrap_land = 0;
    asm volatile("call *%[stub]"
                 :
                 : [stub] "r"(stubs[0])
                 : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
                   "memory", "cc");
    check(g_wrap_land == 2, "wrapfix exec bit0: ASCII -> untaken (skip wrap)");
    flags[0] = 1; g_wrap_land = 0;
    asm volatile("call *%[stub]"
                 :
                 : [stub] "r"(stubs[0])
                 : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
                   "memory", "cc");
    check(g_wrap_land == 1, "wrapfix exec bit0: CJK -> taken (wrap block)");

    // --- bit3 RTS form (rbp node, plain branch)
    flags[3] = 0; run_rbp(stubs[3], node_glyph);
    check(g_wrap_land == 2, "wrapfix exec bit3: h!=0 ASCII -> untaken");
    flags[3] = 1; run_rbp(stubs[3], node_glyph);
    check(g_wrap_land == 1, "wrapfix exec bit3: h!=0 CJK -> taken");
    flags[3] = 0; run_rbp(stubs[3], node_space);
    check(g_wrap_land == 1, "wrapfix exec bit3: h==0 -> taken regardless of flag");

    // --- bit4 R3D form (rbx node)
    auto run_rbx = [&](const uint16_t* node) {
        g_wrap_land = 0;
        asm volatile(
            "mov %[stub], %%r11\n\t"
            "push %%rbx\n\t"
            "mov %[node], %%rbx\n\t"
            "call *%%r11\n\t"
            "pop %%rbx"
            :
            : [node] "r"(node), [stub] "r"(stubs[4])
            : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
              "memory", "cc");
    };
    flags[4] = 0; run_rbx(node_glyph);
    check(g_wrap_land == 2, "wrapfix exec bit4: h!=0 ASCII -> untaken");
    flags[4] = 1; run_rbx(node_glyph);
    check(g_wrap_land == 1, "wrapfix exec bit4: h!=0 CJK -> taken");
    flags[4] = 0; run_rbx(node_space);
    check(g_wrap_land == 1, "wrapfix exec bit4: h==0 -> taken");

    // --- bit1 R2T: routing + replay of mov 0x2900(%rsp),%bpl on untaken path
    {
        uint64_t out_rbp = 0;
        flags[1] = 0; g_wrap_land = 0;
        // NOTE: the "=m" store must run with rsp back at the asm-entry value
        // (GCC renders memory operands rsp-relative): stage the stub-written
        // rbp into r11 across the pop.
        asm volatile(
            "mov %[stub], %%r11\n\t"
            "push %%rbp\n\t"
            "mov %[node], %%rbp\n\t"
            "sub $0x2A00, %%rsp\n\t"
            "movb $0x7E, 0x28F8(%%rsp)\n\t"   // stub rsp + 0x2900 (call pushed 8)
            "call *%%r11\n\t"
            "add $0x2A00, %%rsp\n\t"
            "mov %%rbp, %%r11\n\t"
            "pop %%rbp\n\t"
            "mov %%r11, %[out]"
            : [out] "=m"(out_rbp)
            : [node] "r"(static_cast<const void*>(node_glyph)), [stub] "r"(stubs[1])
            : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
              "memory", "cc");
        check(g_wrap_land == 2, "wrapfix exec bit1: h!=0 ASCII -> untaken");
        const uint64_t np = reinterpret_cast<uint64_t>(node_glyph);
        {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "wrapfix exec bit1: replay mov 0x2900(%%rsp),%%bpl executed"
                          " (out_rbp=0x%llX node=0x%llX)",
                          static_cast<unsigned long long>(out_rbp),
                          static_cast<unsigned long long>(np));
            check((out_rbp & 0xFFull) == 0x7E
                      && (out_rbp & ~0xFFull) == (np & ~0xFFull), buf);
        }
        flags[1] = 1; run_rbp(stubs[1], node_glyph);
        check(g_wrap_land == 1, "wrapfix exec bit1: CJK -> taken");
        flags[1] = 0; run_rbp(stubs[1], node_space);
        check(g_wrap_land == 1, "wrapfix exec bit1: h==0 -> taken");
    }

    // --- bit2 GW: commit form (r13d <- eax on taken/commit path)
    {
        uint32_t out_r13 = 0;
        auto run_gw = [&](const uint16_t* node) {
            g_wrap_land = 0; out_r13 = 0xDeadBeef;
            asm volatile(
                "mov %[stub], %%r11\n\t"
                "push %%rbp\n\t"
                "mov %[node], %%rbp\n\t"
                "mov $0x1234, %%eax\n\t"
                "xor %%r13d, %%r13d\n\t"
                "call *%%r11\n\t"
                "pop %%rbp\n\t"
                "mov %%r13d, %[out]"
                : [out] "=m"(out_r13)
                : [node] "r"(node), [stub] "r"(stubs[2])
                : "rax", "r13", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10",
                  "r11", "memory", "cc");
        };
        flags[2] = 0; run_gw(node_glyph);
        check(g_wrap_land == 2 && out_r13 == 0,
              "wrapfix exec bit2: ASCII -> resume, r13d untouched");
        flags[2] = 1; run_gw(node_glyph);
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "wrapfix exec bit2: CJK -> commit r13d=eax then resume"
                          " (land=%d r13=0x%X)", g_wrap_land, out_r13);
            check(g_wrap_land == 2 && out_r13 == 0x1234, buf);
        }
        flags[2] = 0; run_gw(node_space);
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "wrapfix exec bit2: h==0 -> commit regardless of flag"
                          " (land=%d r13=0x%X)", g_wrap_land, out_r13);
            check(g_wrap_land == 2 && out_r13 == 0x1234, buf);
        }
    }

    std::memset(flags, 0, sizeof(flags));
    bind_flags_for_test(nullptr);
}

} // namespace eu4cjk_test
