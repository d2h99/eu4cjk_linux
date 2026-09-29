#include "selftest_h.h"

#include "../src/escape.h"
#include "../src/savefix.h"

// M5 escape_tool converters: protocol-verbatim ports checked against
// live captures (德 = 12 B7 56 from kam29; 中 = 10 2D 4E from the M3
// selftest; 奥斯曼 = 10 65 59 20 ... from kam28) plus round-trip
// identities and the boundary classes (PUA shift, dead zone, CP1252
// block, surrogate pairs).

namespace eu4cjk_test {

namespace escape = eu4cjk::escape;
namespace savefix = eu4cjk::savefix;

namespace {

bool bytes_eq(const std::string& got, const char* want, size_t want_n,
              const char* tag)
{
    bool ok = got.size() == want_n
              && std::memcmp(got.data(), want, want_n) == 0;
    if (!ok) {
        char buf[256];
        size_t o = static_cast<size_t>(
            std::snprintf(buf, sizeof(buf), "%s (got %zuB:", tag, got.size()));
        for (size_t i = 0; i < got.size() && o + 4 < sizeof(buf); ++i)
            o += static_cast<size_t>(
                std::snprintf(buf + o, sizeof(buf) - o, " %02x",
                              static_cast<unsigned>(
                                  static_cast<uint8_t>(got[i]))));
        std::snprintf(buf + o, sizeof(buf) - o, ")");
        check(false, buf);
        return false;
    }
    check(true, tag);
    return true;
}

// in-place engine-CString helpers against a libstdc++-layout object
void run_cstr_tests()
{
    std::string obj = std::string("\x10\x65\x59\x20\x10\xAF\x65", 7); // 奥␣斯
    check(escape::cstr_to_utf8(&obj), "escape: cstr_to_utf8 converts escaped");
    check(obj == "\xE5\xA5\xA5\x20\xE6\x96\xAF", "escape: cstr_to_utf8 bytes");
    check(!escape::cstr_to_utf8(&obj), "escape: cstr_to_utf8 idempotent skip");

    std::string plain = "autosave.eu4";
    check(!escape::cstr_to_utf8(&plain) && plain == "autosave.eu4",
          "escape: ascii cstr untouched");

    std::string disk = std::string("\xE5\xBA\xB7\xE5\xBE\xB7", 6); // 康德
    check(escape::cstr_to_escaped(&disk), "escape: cstr_to_escaped converts");
    check(disk.size() == 6 && std::memcmp(disk.data(),
                                          "\x10\xB7\x5E\x12\xB7\x56", 6) == 0,
          "escape: cstr_to_escaped bytes (康熙/德 vectors)");

    std::string escaped_junk = std::string("\x10\xB7\x5E", 3); // 康 escaped
    check(!escape::cstr_to_escaped(&escaped_junk) && escaped_junk.size() == 3,
          "escape: already-escaped never double-converted");
}

void run_vector_tests()
{
    // live-capture vectors (escape -> utf8)
    std::string r = escape::to_utf8("\x12\xB7\x56", 3);
    check(r == "\xE5\xBE\xB7", "escape: 德 12 b7 56 -> utf8");
    r = escape::to_utf8("\x10\x2D\x4E", 3);
    check(r == "\xE4\xB8\xAD", "escape: 中 10 2d 4e -> utf8");
    // utf8 -> escape must reproduce the exact capture bytes
    r = escape::to_escaped("\xE5\xA5\xA5\xE6\x96\xAF\xE6\x9B\xBC", 9); // 奥斯曼
    // 斯 U+65AF: low=0xAF high=0x65 -> payload (AF, 65); the special
    // high/low table only biases the marker, never reorders the pair.
    const uint8_t want_osm[] = {0x10, 0x65, 0x59, 0x10, 0xAF, 0x65,
                                0x10, 0xFC, 0x66};
    bytes_eq(r, reinterpret_cast<const char*>(want_osm), sizeof(want_osm),
             "escape: 奥斯曼 utf8 -> capture bytes");

    // 康 U+5EB7 (kam27 capture class)
    r = escape::to_escaped("\xE5\xBA\xB7", 3);
    const uint8_t want_kang[] = {0x10, 0xB7, 0x5E};
    bytes_eq(r, reinterpret_cast<const char*>(want_kang), sizeof(want_kang),
             "escape: 康 utf8 -> 10 b7 5e");

    // dead zone: id 0x200 -> U+2026
    r = escape::to_utf8("\x10\x00\x02", 3);
    check(r == "\xE2\x80\xA6", "escape: dead zone id -> U+2026");

    // PUA shift: U+0100 -> 0xE100; low byte 0x00 is itself special,
    // so marker 0x11 with low+14 (0x0E) - round-trips exactly.
    r = escape::to_escaped("\xC4\x80", 2);
    const uint8_t want_pua[] = {0x11, 0x0E, 0x01};
    bytes_eq(r, reinterpret_cast<const char*>(want_pua), sizeof(want_pua),
             "escape: U+0100 -> PUA-shifted 0x11 triple");
    r = escape::to_utf8("\x11\x0E\x01", 3);
    check(r == "\xC4\x80", "escape: PUA triple -> U+0100");

    // 0x12 marker from special high byte: U+5FB7 high 0x5F ('_')
    r = escape::to_utf8("\x12\xB7\x56", 3);
    check(r == "\xE5\xBE\xB7", "escape: 0x12 bias reverses exactly");

    // ASCII passthrough + CP1252 high byte (0xA9 copyright in latin1)
    r = escape::to_utf8("ab\xA9", 3);
    check(r == "ab\xC2\xA9", "escape: cp1252 0xA9 -> U+00A9 utf8");

    // has_escapes / utf8 sniffers
    check(escape::has_escapes("a\x10" "b", 3) == false,
          "escape: marker needs 2 payload bytes");
    check(escape::has_escapes("\x10\x65\x59", 3), "escape: triple detected");
    check(escape::is_valid_utf8_multibyte("\xE5\xA5\xA5", 3),
          "escape: valid utf8 multibyte");
    check(!escape::is_valid_utf8_multibyte("\x10\x65\x59", 3),
          "escape: escaped bytes fail utf8 sniff");
    check(!escape::is_valid_utf8_multibyte("plain", 5),
          "escape: pure ascii is not multibyte");

    // round-trip identity for a mixed realistic save name
    const char* names[] = {
        "\xE5\xA5\xA5\xE6\x96\xAF\xE6\x9B\xBC",             // 奥斯曼
        "\xE8\xA5\xBF\xE8\x97\x8F",                         // 西藏
        "\xE5\xA4\xA7\xE6\xB8\x85\x45\x75\x34",             // 大清Eu4
        "\xE5\xBA\xB7\xE7\x86\x99\x34\x34\x2E\x65\x34",     // 康熙44.e4
    };
    for (const char* nm : names) {
        std::string esc = escape::to_escaped(nm, std::strlen(nm));
        std::string back = escape::to_utf8(esc.data(), esc.size());
        check(back == nm, "escape: round-trip identity");
    }
}

} // namespace

// ---- savename stub execution (M5 write boundary) ---------------------------
//
// Drives the real stub bytes: helper converts in place and returns true
// for escaped strings (stub must jump straight to the function-ret
// gadget), false otherwise (stub must replay mov rax,[rdi+8]; test and
// land on the resume gadget with rax == len).
namespace {
int g_save_helper_calls = 0;
uintptr_t g_ret_seen = 0;
__attribute__((noinline)) bool save_test_helper(void* cstr, uintptr_t ret_site)
{
    ++g_save_helper_calls;
    g_ret_seen = ret_site;
    return escape::cstr_to_utf8(cstr);
}
__attribute__((noinline)) void save_ret_gadget() { g_save_stub_landed = 1; }
__attribute__((noinline)) void save_resume_gadget() { g_save_stub_landed = 2; }
__attribute__((noinline)) void header_test_helper(void* info)
{
    auto* base = static_cast<uint8_t*>(info);
    for (size_t off : {0x8, 0x28, 0xA0, 0x168})
        escape::cstr_to_escaped(base + off);
}
int g_title_helper_calls = 0;
__attribute__((noinline)) void title_test_helper(void* name)
{
    ++g_title_helper_calls;
    escape::cstr_to_escaped(name);
}
int g_row_helper_calls = 0;
const void* g_row_widget = nullptr;
const void* g_row_name = nullptr;
__attribute__((noinline)) void row_test_helper(void* widget, void* name)
{
    ++g_row_helper_calls;
    g_row_widget = widget;
    g_row_name = name;
    escape::cstr_to_escaped(name);
}

std::string g_row_real_seen;
bool g_row_real_called = false;
void row_real_fake(void*, void* n, int)
{
    g_row_real_called = true;
    g_row_real_seen = *static_cast<std::string*>(n);
}

// The savetitle stub replays the ctor's three pushes (rbp, r15, r14)
// before jumping to the resume point; the game's ctor body balances
// them, a test resume must pop them before returning to the driver.
extern "C" void title_resume_trampoline();
asm(".text\n"
    ".globl title_resume_trampoline\n"
    "title_resume_trampoline:\n"
    "  popq %r14\n"
    "  popq %r15\n"
    "  popq %rbp\n"
    "  ret\n");
}

void run_escape_tests()
{
    run_vector_tests();
    run_cstr_tests();

    // ---- site-aware dispatch (write boundary v2) --------------------------
    {
        std::string d = std::string("\x10\x65\x59", 3);          // 奥 escaped
        check(savefix::savename_dispatch(&d, savefix::kRetSaveName)
                  && d == std::string("\xE5\xA5\xA5", 3),
              "savefix: dispatch save-name site escaped->utf8 + skip");

        d = std::string("\xE5\xA5\xA5", 3);                      // utf8 at save-name site
        check(savefix::savename_dispatch(&d, savefix::kRetSaveName)
                  && d == std::string("\xE5\xA5\xA5", 3),
              "savefix: dispatch save-name site keeps utf8 as-is");

        d = std::string("\xE5\xA5\xA5\xE6\x96\xAF\xE6\x9B\xBC", 9); // 奥斯曼 utf8
        check(savefix::savename_dispatch(&d, savefix::kRetPrefillA)
                  && d == std::string("\x10\x65\x59\x10\xAF\x65\x10\xFC\x66", 9),
              "savefix: dispatch prefill site utf8->escaped");
        check(savefix::savename_dispatch(&d, savefix::kRetPrefillB)
                  && d == std::string("\x10\x65\x59\x10\xAF\x65\x10\xFC\x66", 9),
              "savefix: dispatch prefill site keeps escaped as-is");

        d = std::string("\xE5\xA5\xA5", 3);
        check(savefix::savename_dispatch(&d, 0x42424200)
                  && d == std::string("\xE5\xA5\xA5", 3),
              "savefix: dispatch unknown site protects utf8");

        d = std::string("\x10\x65\x59", 3);
        check(savefix::savename_dispatch(&d, 0x42424200)
                  && d == std::string("\x10\x65\x59", 3),
              "savefix: dispatch unknown site never folds escaped");

        d = "autosave.eu4";
        check(!savefix::savename_dispatch(&d, savefix::kRetSaveName),
              "savefix: dispatch ascii replays vanilla");

        d = "";
        check(!savefix::savename_dispatch(&d, 0x42424200),
              "savefix: dispatch empty string replays");
    }

    uint8_t bytes[96];
    const size_t len = savefix::build_savename_stub(
        bytes, sizeof(bytes), reinterpret_cast<uintptr_t>(&save_test_helper),
        reinterpret_cast<uintptr_t>(&save_resume_gadget),
        reinterpret_cast<uintptr_t>(&save_ret_gadget));
    check(len > 0, "savefix: savename builder length");
    const uintptr_t stub = eu4cjk::stubgen::emit_raw(bytes, len);
    check(stub != 0, "savefix: savename emit_raw");
    if (!stub) return;

    // escaped name: handled path -> ret gadget, string converted
    std::string name = std::string("\x10\xB7\x5E\x2E\x65\x34", 6); // 康.e4
    g_save_stub_landed = 0;
    g_save_helper_calls = 0;
    // sub/add $0x100 skips the red zone for the deep C++ call inside
    asm volatile("sub $0x100, %%rsp\n\t"
                 "call *%0\n\t"
                 "add $0x100, %%rsp"
                 :: "r"(stub), "D"(&name)
                 : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11",
                   "memory", "cc");
    check(g_save_stub_landed == 1 && g_save_helper_calls == 1,
          "savefix: escaped name -> ret path via helper");
    check(name == std::string("\xE5\xBA\xB7\x2E\x65\x34", 6),
          "savefix: escaped name converted to utf8");
    check(g_ret_seen >= 0x400000 && g_ret_seen < 0x600000,
          "savefix: stub forwards caller return address in rsi");

    // ascii name: replay path -> resume gadget, rax == len from replay
    std::string ascii = "autosave.eu4";
    register uint64_t rax_out asm("rax");
    uint64_t got_rax = 0;
    g_save_stub_landed = 0;
    asm volatile("sub $0x100, %%rsp\n\t"
                 "call *%2\n\t"
                 "add $0x100, %%rsp\n\t"
                 "movq %%rax, %0"
                 : "=r"(got_rax)
                 : "D"(&ascii), "r"(stub)
                 : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11",
                   "memory", "cc");
    (void)rax_out;
    check(g_save_stub_landed == 2 && g_save_helper_calls == 2,
          "savefix: ascii name -> replay/resume path");
    check(got_rax == ascii.size(),
          "savefix: replayed mov rax,[rdi+8] restores len");
    check(ascii == "autosave.eu4", "savefix: ascii name untouched");

    // ---- read boundary stub: fake CSaveHeaderInfo with the 4 fields ----
    struct FakeInfo {
        char pad0[0x8];
        std::string f8;         // +0x08
        std::string f28;        // +0x28
        char pad2[0x58];
        std::string fa0;        // +0xA0
        char pad3[0xA8];
        std::string f168;       // +0x168
    };
    static_assert(offsetof(FakeInfo, f8) == 0x8, "layout");
    static_assert(offsetof(FakeInfo, f28) == 0x28, "layout");
    static_assert(offsetof(FakeInfo, fa0) == 0xA0, "layout");
    static_assert(offsetof(FakeInfo, f168) == 0x168, "layout");
    FakeInfo info;
    info.f8 = "Ottoman";                        // ascii tag: untouched
    info.f28 = std::string("\xE5\xA5\xA5\xE6\x96\xAF\xE6\x9B\xBC", 9); // 奥斯曼 utf8
    info.fa0 = std::string("\x10\x65\x59", 3);   // already escaped: no-op
    info.f168 = "";                             // empty: no-op

    const size_t hlen = savefix::build_saveheader_stub(
        bytes, sizeof(bytes), reinterpret_cast<uintptr_t>(&header_test_helper),
        reinterpret_cast<uintptr_t>(&save_resume_gadget));
    check(hlen > 0, "savefix: saveheader builder length");
    const uintptr_t hstub = eu4cjk::stubgen::emit_raw(bytes, hlen);
    check(hstub != 0, "savefix: saveheader emit_raw");
    if (hstub) {
        uint64_t rax_sentinel = 0xA11CE5;
        uint64_t got = 0;
        g_save_stub_landed = 0;
        // set rbx inside the template: the stub reads info from rbx (the
        // engine register at the site); register-asm bindings are not
        // guaranteed for +r operands, so feed it via a plain input.
        asm volatile("movq %2, %%rbx\n\t"
                     "sub $0x108, %%rsp\n\t"
                     "call *%3\n\t"
                     "add $0x108, %%rsp\n\t"
                     "movq %%rax, %0"
                     : "=r"(got)
                     : "a"(rax_sentinel), "r"(&info), "r"(hstub)
                     : "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9",
                       "r10", "r11", "r15", "memory", "cc");
        check(g_save_stub_landed == 2, "savefix: header stub -> resume");
        check(got == rax_sentinel, "savefix: header stub preserves rax");
        check(info.f8 == "Ottoman", "savefix: ascii meta field untouched");
        check(info.f28 == std::string("\x10\x65\x59\x10\xAF\x65\x10\xFC\x66", 9),
              "savefix: utf8 meta title -> escaped");
        check(info.fa0 == std::string("\x10\x65\x59", 3),
              "savefix: escaped meta field no-op");
    }

    // ---- list-title stub: CSavegameItem ctor entry, name in rcx ----
    {
        std::string title = std::string("\xE5\xA5\xA5" "222", 6); // 奥222 utf8
        uint64_t got_s = 0, got_d = 0;
        g_title_helper_calls = 0;
        const size_t tlen = savefix::build_savetitle_stub(
            bytes, sizeof(bytes),
            reinterpret_cast<uintptr_t>(&title_test_helper),
            reinterpret_cast<uintptr_t>(&title_resume_trampoline));
        check(tlen > 0, "savefix: savetitle builder length");
        const uintptr_t tstub = eu4cjk::stubgen::emit_raw(bytes, tlen);
        check(tstub != 0, "savefix: savetitle emit_raw");
        if (tstub) {
            // ctor-entry parity: function entry rsp%16==8; the driver sits
            // at %16==0, so sub $0x100 + call reproduces it. rsi/rdx carry
            // sentinel bools the ctor body reads after the resume point;
            // they are loaded as immediates so GCC cannot co-locate the
            // sentinels with the output registers.
            asm volatile("sub $0x100, %%rsp\n\t"
                         "movq $0x1111, %%rsi\n\t"
                         "movq $0x2222, %%rdx\n\t"
                         "call *%3\n\t"
                         "add $0x100, %%rsp\n\t"
                         "movq %%rsi, %0\n\t"
                         "movq %%rdx, %1\n\t"
                         : "=m"(got_s), "=m"(got_d)
                         : "D"(0xF00D), "r"(tstub), "c"(&title)
                         : "rax", "r8", "r9", "r10", "r11", "r15",
                           "memory", "cc");
            check(g_title_helper_calls == 1,
                  "savefix: title stub called helper once");
            check(title == std::string("\x10\x65\x59" "222", 6),
                  "savefix: title utf8 -> escaped in place");
            check(got_s == 0x1111 && got_d == 0x2222,
                  "savefix: title stub preserves rsi/rdx for ctor body");
        }
    }

    // ---- row-label: rowname_pass semantics (backup/convert/restore) ----
    {
        g_row_real_called = false;
        std::string n1 = std::string("\xE5\x8C\x88" "1561", 7); // 匈1561 utf8
        const bool c1 = savefix::rowname_pass((void*)0x77, &n1, row_real_fake);
        check(c1 && g_row_real_called, "savefix: rowname converts utf8 name");
        check(g_row_real_seen == std::string("\x10\x08\x53" "1561", 7),
              "savefix: rowname real call saw escaped text");
        check(n1 == std::string("\xE5\x8C\x88" "1561", 7),
              "savefix: rowname restores caller's string");

        std::string n2 = "autosave.eu4";
        const bool c2 = savefix::rowname_pass((void*)0x77, &n2, row_real_fake);
        check(!c2 && g_row_real_seen == "autosave.eu4",
              "savefix: rowname ascii passes through untouched");
    }

    // ---- row-label stub execution (site A variant) ----
    {
        std::string name = std::string("\xE5\x8C\x88" "1561", 7);
        const uint8_t mov_rdi[3] = {0x48, 0x89, 0xC7}; // rax -> rdi
        const uint8_t mov_rsi[3] = {0x4C, 0x89, 0xFE}; // r15 -> rsi
        g_row_helper_calls = 0;
        const size_t rlen = savefix::build_rowname_stub(
            bytes, sizeof(bytes),
            reinterpret_cast<uintptr_t>(&row_test_helper),
            reinterpret_cast<uintptr_t>(&save_resume_gadget),
            mov_rdi, mov_rsi);
        check(rlen > 0, "savefix: rowname builder length");
        const uintptr_t rstub = eu4cjk::stubgen::emit_raw(bytes, rlen);
        check(rstub != 0, "savefix: rowname emit_raw");
        if (rstub) {
            register uint64_t r15_v asm("r15");
            register uint64_t rax_v asm("rax");
            (void)r15_v; (void)rax_v;
            g_save_stub_landed = 0;
            // site parity rsp%16==0 at stub entry: driver sub $0x108 then
            // call reproduces it; r15 carries the name, rax the widget.
            // NB: the lea must run BEFORE the sub: a frame-relative "m"
            // operand is encoded against the rsp GCC believes is current,
            // and GCC cannot see the template's own rsp adjustment.
            asm volatile("lea %1, %%r15\n\t"
                         "sub $0x108, %%rsp\n\t"
                         "mov $0x777, %%eax\n\t"
                         "call *%0\n\t"
                         "add $0x108, %%rsp"
                         :: "r"(rstub), "m"(name)
                         : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9",
                           "r10", "r11", "r15", "memory", "cc");
            check(g_row_helper_calls == 1,
                  "savefix: rowname stub called helper once");
            check(g_row_widget == (const void*)0x777,
                  "savefix: rowname stub forwards widget (rax)");
            check(g_row_name == &name,
                  "savefix: rowname stub forwards name (r15)");
            check(name == std::string("\x10\x08\x53" "1561", 7),
                  "savefix: rowname stub converts in place");
            check(g_save_stub_landed == 2,
                  "savefix: rowname stub reaches resume");
        }
    }
}

} // namespace eu4cjk_test
