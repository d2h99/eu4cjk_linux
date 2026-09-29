#include "savefix.h"
#include "escape.h"
#include "log.h"
#include "stubgen.h"

#include <atomic>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <string>

namespace eu4cjk::savefix {

std::atomic<uint64_t> g_name_to_utf8{0};
std::atomic<uint64_t> g_name_to_escaped{0};
std::atomic<uint64_t> g_name_protected{0};
std::atomic<uint64_t> g_name_untouched{0};
std::atomic<uint64_t> g_header_to_escaped{0};
std::atomic<uint64_t> g_title_to_escaped{0};

namespace {

// C thunk for the RemoveSpecialCharacters entry hook (this in rdi, the
// caller's return address in rsi, forwarded from [rsp] by the stub).
extern "C" bool eu4cjk_savename_clean(void* cstr, uintptr_t ret_site)
{
    return savename_dispatch(cstr, ret_site);
}

// CString::RemoveSpecialCharacters (0x254d05a, 30 bytes, void return):
//   254d05a: 48 8b 47 08       mov 0x8(%rdi),%rax     len
//   254d05e: 85 c0             test %eax,%eax
//   254d060: 74 55             je +0x55 (ret)
//   ... per-byte fold loop with the 256-entry table @0x28b7d60 ...
//   254d0b7: c3                ret
// Hook site: the first 6 bytes (mov+test); resume 0x254d060 re-enters at
// the je, whose flags the replayed test re-establishes. Handled path
// jumps straight to the function's ret (0x254d0b7).
constexpr uintptr_t kSiteClean = 0x254d05a;
constexpr size_t kLenClean = 6;
constexpr uint8_t kOrigClean[kLenClean] = {
    0x48, 0x8B, 0x47, 0x08,
    0x85, 0xC0,
};
constexpr uintptr_t kResumeClean = 0x254d060;
constexpr uintptr_t kRetClean = 0x254d0b7;

// Hand-assembled (r11 absolute calls; no rel32 to fix except the two
// tail jumps). Layout:
//   00: 50                   push %rax        (align for the call)
//   01: 49 BB <helper>       mov r11, helper
//   0B: 48 89 FE? no - rdi already holds this
//   0B: 41 FF D3             call *%r11
//   0E: 84 C0                test %al,%al
//   10: 58                   pop %rax         (flags intact)
//   11: 75 0D                jne handled      -> +0x20
//   13: <replay 6 bytes>     mov+test (re-establishes rax and flags)
//   19: E9 <rel32>           jmp 0x254d060
//   1E: 90                   nop (pad)
//   1F: 90                   nop
//   20: handled: E9 <rel32>  jmp 0x254d0b7 (ret)

bool install_savename_fix()
{
    uint8_t s[96];
    const size_t len = build_savename_stub(
        s, sizeof(s), reinterpret_cast<uintptr_t>(&eu4cjk_savename_clean),
        kResumeClean, kRetClean);
    if (!len) {
        log_line("[eu4cjk] savefix: savename builder overflow\n");
        return false;
    }
    const uintptr_t stub = stubgen::emit_raw(s, len);
    if (!stub || !stubgen::install_jmp(kSiteClean, kOrigClean, kLenClean,
                                       stub)) {
        log_line("[eu4cjk] savefix: savename install failed\n");
        return false;
    }
    log_line("[eu4cjk] savefix: savename stub @ 0x%lx (site 0x%lx)\n",
             static_cast<unsigned long>(stub),
             static_cast<unsigned long>(kSiteClean));
    return true;
}
// C helper for the read-boundary hook: convert the four meta CString
// fields (+0x8/+0x28/+0xA0/+0x168, from CSaveHeaderInfo::ReadFileHeader's
// CReader::Read(CString&) sites) UTF-8 -> escaped so every display
// consumer (list title, tooltips, continue button) renders CJK.
extern "C" void eu4cjk_saveheader_fix(void* info)
{
    auto* base = static_cast<uint8_t*>(info);
    for (size_t off : {0x8, 0x28, 0xA0, 0x168}) {
        if (escape::cstr_to_escaped(base + off))
            g_header_to_escaped.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace
// Site-aware write-boundary dispatch. The old single-point rule ("any
// escaped string -> UTF-8") corrupted the prefill: SaveGameSelect feeds
// the default name through RSC straight into the CEditBox, so the UTF-8
// we produced displayed as mojibake and was folded to "a??" garbage when
// saved. Direction now depends on the caller:
//   kRetSaveName - the name read from the widget on save: escaped ->
//                  UTF-8 so the filename (and header title) land as
//                  real UTF-8;
//   kRetPrefill* - the default name on its way into the widget: UTF-8 ->
//                  escaped so the font pipeline renders CJK;
//   any site     - escaped strings and valid UTF-8 are NEVER folded
//                  (BuildSaveFileFolder/BuildFullSavePath re-run RSC on
//                  the assembled path; the vanilla fold collapses
//                  non-CP1252 text to ASCII garbage).
bool savename_dispatch(void* cstr, uintptr_t ret_site)
{
    auto* s = static_cast<std::string*>(cstr);
    const char* p = s->data();
    const size_t n = s->size();
    if (escape::has_escapes(p, n)) {
        if (ret_site == kRetSaveName && escape::cstr_to_utf8(cstr))
            g_name_to_utf8.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (escape::is_valid_utf8_multibyte(p, n)) {
        if ((ret_site == kRetPrefillA || ret_site == kRetPrefillB)
            && escape::cstr_to_escaped(cstr)) {
            g_name_to_escaped.fetch_add(1, std::memory_order_relaxed);
        }
        g_name_protected.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_name_untouched.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// CSavegameItem::SetupFromFileHeader +0x2b: right after
//   1d2c002: call CSaveHeaderInfo::ReadFileHeader
//   1d2c007: 41 89 C7    mov %eax,%r15d     <- hook site (5 bytes incl.)
//   1d2c00a: 84 C0       test %al,%al
//   1d2c00c: 0f 84 ..    je ...             <- resume
// rbx = &item->info (this+0xa0, set at 1d2bfff) and is live across the
// call; rax (the bool return) must survive OUR helper call, so the stub
// saves it. At this point rsp%16==0 (6 pushes + sub $0x88 from entry),
// hence sub $0x10 keeps the helper's call ABI aligned.
constexpr uintptr_t kSiteHeader = 0x1d2c007;
constexpr size_t kLenHeader = 5;
constexpr uint8_t kOrigHeader[kLenHeader] = {0x41, 0x89, 0xC7, 0x84, 0xC0};
constexpr uintptr_t kResumeHeader = 0x1d2c00c;

size_t build_saveheader_stub(uint8_t* out, size_t cap, uintptr_t helper,
                             uintptr_t resume)
{
    size_t k = 0;
    auto put = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) out[k++] = p[i];
    };
    auto mov_r11 = [&](uintptr_t v) {
        const uint8_t op[] = {0x49, 0xBB};
        put(op, 2);
        std::memcpy(out + k, &v, 8);
        k += 8;
    };
    if (cap < 53) return 0;
    const uint8_t pro[] = {0x48, 0x83, 0xEC, 0x10,        // sub $0x10,%rsp
                           0x48, 0x89, 0x44, 0x24, 0x08}; // mov %rax,0x8(%rsp)
    put(pro, sizeof(pro));                           // 00
    const uint8_t mov_rdi[] = {0x48, 0x89, 0xDF};    // mov %rbx,%rdi (REX.W only!)
    put(mov_rdi, 3);                                 // 09
    mov_r11(helper);                                 // 0C
    const uint8_t call_r11[] = {0x41, 0xFF, 0xD3};
    put(call_r11, 3);                                // 16
    const uint8_t rest[] = {0x48, 0x8B, 0x44, 0x24, 0x08, // mov 0x8(%rsp),%rax
                            0x48, 0x8D, 0x64, 0x24, 0x10};// lea 0x10(%rsp),%rsp
    put(rest, sizeof(rest));                         // 19
    put(kOrigHeader, kLenHeader);                    // 1E..22 (replay)
    mov_r11(resume);                                 // 23
    const uint8_t jmp_r11[] = {0x41, 0xFF, 0xE3};
    put(jmp_r11, 3);                                 // 2D
    return k;
}

bool install_header_fix()
{
    uint8_t s[96];
    const size_t len = build_saveheader_stub(
        s, sizeof(s), reinterpret_cast<uintptr_t>(&eu4cjk_saveheader_fix),
        kResumeHeader);
    if (!len) {
        log_line("[eu4cjk] savefix: saveheader builder overflow\n");
        return false;
    }
    const uintptr_t stub = stubgen::emit_raw(s, len);
    if (!stub || !stubgen::install_jmp(kSiteHeader, kOrigHeader, kLenHeader,
                                       stub)) {
        log_line("[eu4cjk] savefix: saveheader install failed\n");
        return false;
    }
    log_line("[eu4cjk] savefix: saveheader stub @ 0x%lx (site 0x%lx)\n",
             static_cast<unsigned long>(stub),
             static_cast<unsigned long>(kSiteHeader));
    return true;
}

// ---- list-title boundary: CSavegameItem ctor -------------------------------
//
// The load/save lists (frontend CGameSetup::RefreshLocalSave, in-game
// CIngameSaveLoadMenuBase::RefreshList, ironman CIronmanSaveSelect) all
// construct CLocalSavegameItem entries whose 4th ctor argument flows into
// the CSavegameItem base and verbatim into the CStandardlistboxItem row
// text. That string is filename-derived, i.e. REAL UTF-8 on disk thanks
// to the write boundary - and would render as mojibake through the
// escaped font pipeline (byte 0xA5 -> the font's yen glyph: "￥￥222").
// Converting it UTF-8 -> escaped at the base ctor fixes every list in
// one point; the item's filename (CBasicSaveInfo at +0x228, fed from a
// different ctor argument) is never touched, so file operations keep
// using the real UTF-8 path.
//
// CSavegameItem::CSavegameItem(bool, bool, CString const&) @0x1d2bac8:
//   1d2bac8: 55        push %rbp
//   1d2bac9: 41 57     push %r15
//   1d2bacb: 41 56     push %r14     <- exactly 5 bytes, resume 0x1d2bacd
//   1d2bacd: 53        push %rbx
//   1d2bace: 50        push %rax
//   1d2bacf: 41 89 d6  mov %edx,%r14d   <- bool2 read AFTER resume
//   1d2bad2: 89 f5     mov %esi,%ebp    <- bool1 read AFTER resume
// so the helper (a full SysV call) must preserve rsi/rdx as well as the
// other argument registers.
constexpr uintptr_t kSiteTitle = 0x1d2bac8;
constexpr size_t kLenTitle = 5;
constexpr uint8_t kOrigTitle[kLenTitle] = {0x55, 0x41, 0x57, 0x41, 0x56};
constexpr uintptr_t kResumeTitle = 0x1d2bacd;

extern "C" void eu4cjk_savetitle_fix(void* name)
{
    if (escape::cstr_to_escaped(name))
        g_title_to_escaped.fetch_add(1, std::memory_order_relaxed);
}

size_t build_savetitle_stub(uint8_t* out, size_t cap, uintptr_t helper,
                            uintptr_t resume)
{
    size_t k = 0;
    auto put = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) out[k++] = p[i];
    };
    auto mov_r11 = [&](uintptr_t v) {
        const uint8_t op[] = {0x49, 0xBB};
        put(op, 2);
        std::memcpy(out + k, &v, 8);
        k += 8;
    };
    if (cap < 83) return 0;
    // sub $0x28 keeps the helper call aligned (entry rsp%16==8) and
    // leaves 0x28(%rsp) = the caller's return address untouched. All
    // four argument registers are saved: rdi/rsi/rdx are re-read by the
    // ctor body after the resume point, rcx carries the name.
    const uint8_t pro[] = {0x48, 0x83, 0xEC, 0x28,          // sub $0x28,%rsp
                           0x48, 0x89, 0x7C, 0x24, 0x20,    // mov %rdi,0x20(%rsp)
                           0x48, 0x89, 0x74, 0x24, 0x08,    // mov %rsi,0x08(%rsp)
                           0x48, 0x89, 0x54, 0x24, 0x10,    // mov %rdx,0x10(%rsp)
                           0x48, 0x89, 0x4C, 0x24, 0x18,    // mov %rcx,0x18(%rsp)
                           0x48, 0x89, 0xCF};               // mov %rcx,%rdi
    put(pro, sizeof(pro));                           // 00..1A
    mov_r11(helper);                                 // 1B
    const uint8_t call_r11[] = {0x41, 0xFF, 0xD3};
    put(call_r11, 3);                                // 25
    const uint8_t rest[] = {0x48, 0x8B, 0x7C, 0x24, 0x20, // mov 0x20(%rsp),%rdi
                            0x48, 0x8B, 0x74, 0x24, 0x08, // mov 0x08(%rsp),%rsi
                            0x48, 0x8B, 0x54, 0x24, 0x10, // mov 0x10(%rsp),%rdx
                            0x48, 0x8B, 0x4C, 0x24, 0x18, // mov 0x18(%rsp),%rcx
                            0x48, 0x8D, 0x64, 0x24, 0x28, // lea 0x28(%rsp),%rsp
                            0x55, 0x41, 0x57, 0x41, 0x56};// replay 3 pushes
    put(rest, sizeof(rest));                         // 28..50
    mov_r11(resume);                                 // 51
    const uint8_t jmp_r11[] = {0x41, 0xFF, 0xE3};
    put(jmp_r11, 3);                                 // 5B
    return k;
}

bool install_title_fix()
{
    uint8_t s[112];
    const size_t len = build_savetitle_stub(
        s, sizeof(s), reinterpret_cast<uintptr_t>(&eu4cjk_savetitle_fix),
        kResumeTitle);
    if (!len) {
        log_line("[eu4cjk] savefix: savetitle builder overflow\n");
        return false;
    }
    const uintptr_t stub = stubgen::emit_raw(s, len);
    if (!stub || !stubgen::install_jmp(kSiteTitle, kOrigTitle, kLenTitle,
                                       stub)) {
        log_line("[eu4cjk] savefix: savetitle install failed\n");
        return false;
    }
    log_line("[eu4cjk] savefix: savetitle stub @ 0x%lx (site 0x%lx)\n",
             static_cast<unsigned long>(stub),
             static_cast<unsigned long>(kSiteTitle));
    return true;
}

// ---- row-label boundary: CInstantTextBox::ChangeString sites ---------------
//
// The VISIBLE row text of every save list entry is NOT the listbox base
// text (titles_to_escaped stayed 0 while the list showed byte soup):
// CLocalSavegameItem's ctor feeds the raw filename into a per-row
// CInstantTextBox via ChangeString, on one of two branch-dependent
// sites. The third acceptance round made this visible: raw UTF-8
// rendered byte-wise (奥斯曼 -> the font's yen glyphs, 匈牙利 -> all
// glyphs missing, i.e. the CJK part just vanished).
//
// Both sites call ChangeString(widget, name, 0) with name = raw UTF-8
// filename (site A: the enumeration array element in r15 - which the
// caller may reuse, so it must be restored; site B: the item's own
// CBasicSaveInfo CString at this+0x228). The helper therefore runs
// backup -> convert -> real call -> restore, and the caller's string is
// never observably mutated.
//
// CInstantTextBox::ChangeString @0x208b442 (verified both call targets:
// 0x1d2c388+0x35f0ba and 0x1d2c3d0+0x35f072).
//
// Site A (bool=1: frontend GetLocalSaves, in-game RefreshList #1):
//   1d2c37b: 48 89 C7      mov %rax,%rdi      ; widget
//   1d2c37e: 4C 89 FE      mov %r15,%rsi      ; name (caller's array slot)
//   1d2c381: 31 D2         xor %edx,%edx
//   1d2c383: E8 ..         call 0x208b442     ; 13 bytes, resume 0x1d2c388
// Site B (bool=0: in-game RefreshList #2):
//   1d2c3c3: 48 89 EF      mov %rbp,%rdi
//   1d2c3c6: 4C 89 F6      mov %r14,%rsi      ; name (this+0x228 CString)
//   1d2c3c9: 31 D2         xor %edx,%edx
//   1d2c3cb: E8 ..         call 0x208b442     ; 13 bytes, resume 0x1d2c3d0
// At both sites rsp%16==0 (ctor pushed 5 regs from an 8-aligned entry),
// so the stub subs $0x20 to align the helper call.
constexpr uintptr_t kSiteRowA = 0x1d2c37b;
constexpr uintptr_t kSiteRowB = 0x1d2c3c3;
constexpr size_t kLenRow = 13;
constexpr uint8_t kOrigRowA[kLenRow] = {0x48, 0x89, 0xC7, 0x4C, 0x89, 0xFE,
                                        0x31, 0xD2, 0xE8, 0xBA, 0xF0, 0x35,
                                        0x00};
constexpr uint8_t kOrigRowB[kLenRow] = {0x48, 0x89, 0xEF, 0x4C, 0x89, 0xF6,
                                        0x31, 0xD2, 0xE8, 0x72, 0xF0, 0x35,
                                        0x00};
constexpr uintptr_t kResumeRowA = 0x1d2c388;
constexpr uintptr_t kResumeRowB = 0x1d2c3d0;
constexpr uintptr_t kRealChangeString = 0x208B442;
constexpr uint8_t kMovRdiA[3] = {0x48, 0x89, 0xC7}; // mov %rax,%rdi
constexpr uint8_t kMovRsiA[3] = {0x4C, 0x89, 0xFE}; // mov %r15,%rsi
constexpr uint8_t kMovRdiB[3] = {0x48, 0x89, 0xEF}; // mov %rbp,%rdi
constexpr uint8_t kMovRsiB[3] = {0x4C, 0x89, 0xF6}; // mov %r14,%rsi

std::atomic<uint64_t> g_rowname_to_escaped{0};

using RowRealFn = void (*)(void*, void*, int);

// Testable core: backup the caller's string, convert UTF-8 -> escaped,
// hand the converted text to the real ChangeString, then restore the
// original bytes so the caller's (possibly reused) string is untouched.
bool rowname_pass(void* widget, void* name, RowRealFn real)
{
    auto* s = static_cast<std::string*>(name);
    std::string backup = *s;
    const bool conv = escape::cstr_to_escaped(name);
    real(widget, name, 0);
    if (conv) {
        *s = std::move(backup);
        g_rowname_to_escaped.fetch_add(1, std::memory_order_relaxed);
    }
    return conv;
}

extern "C" void eu4cjk_rowname_fix(void* widget, void* name)
{
    // >2GB away from the engine image: GCC must emit an indirect call.
    rowname_pass(widget, name,
                 reinterpret_cast<RowRealFn>(kRealChangeString));
}

size_t build_rowname_stub(uint8_t* out, size_t cap, uintptr_t helper,
                          uintptr_t resume, const uint8_t mov_rdi[3],
                          const uint8_t mov_rsi[3])
{
    size_t k = 0;
    auto put = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) out[k++] = p[i];
    };
    auto mov_r11 = [&](uintptr_t v) {
        const uint8_t op[] = {0x49, 0xBB};
        put(op, 2);
        std::memcpy(out + k, &v, 8);
        k += 8;
    };
    if (cap < 40) return 0;
    const uint8_t pro[] = {0x48, 0x83, 0xEC, 0x20};   // sub $0x20,%rsp
    put(pro, sizeof(pro));                           // 00
    put(mov_rdi, 3);                                 // 04 widget
    put(mov_rsi, 3);                                 // 07 name
    mov_r11(helper);                                 // 0A
    const uint8_t call_r11[] = {0x41, 0xFF, 0xD3};
    put(call_r11, 3);                                // 14
    const uint8_t ep[] = {0x48, 0x83, 0xC4, 0x20};   // add $0x20,%rsp
    put(ep, sizeof(ep));                             // 17
    mov_r11(resume);                                 // 1B
    const uint8_t jmp_r11[] = {0x41, 0xFF, 0xE3};
    put(jmp_r11, 3);                                 // 25
    return k;
}

bool install_rowname_fix(uintptr_t site, const uint8_t* orig,
                         uintptr_t resume, const uint8_t mov_rdi[3],
                         const uint8_t mov_rsi[3], const char* tag)
{
    uint8_t s[64];
    const size_t len = build_rowname_stub(
        s, sizeof(s), reinterpret_cast<uintptr_t>(&eu4cjk_rowname_fix),
        resume, mov_rdi, mov_rsi);
    if (!len) {
        log_line("[eu4cjk] savefix: %s builder overflow\n", tag);
        return false;
    }
    const uintptr_t stub = stubgen::emit_raw(s, len);
    if (!stub || !stubgen::install_jmp(site, orig, kLenRow, stub)) {
        log_line("[eu4cjk] savefix: %s install failed\n", tag);
        return false;
    }
    log_line("[eu4cjk] savefix: %s stub @ 0x%lx (site 0x%lx)\n", tag,
             static_cast<unsigned long>(stub),
             static_cast<unsigned long>(site));
    return true;
}


size_t build_savename_stub(uint8_t* out, size_t cap, uintptr_t helper,
                           uintptr_t resume, uintptr_t fnret)
{
    size_t k = 0;
    auto put = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) out[k++] = p[i];
    };
    auto mov_r11 = [&](uintptr_t v) {
        const uint8_t op[] = {0x49, 0xBB};
        put(op, 2);
        std::memcpy(out + k, &v, 8);
        k += 8;
    };
    if (cap < 72) return 0;
    // rdi (this) is caller-saved and the helper is a full SysV call: it
    // MUST be saved/restored or the replayed mov 0x8(%rdi) and the whole
    // vanilla fold loop read garbage (the M3-T4 GW-rdi lesson). sub/lea
    // $0x18 keeps the ABI alignment (entry rsp%16==8) and lea restores
    // rsp WITHOUT touching the flags set by test al,al. The return
    // address the caller pushed sits at 0x18(%rsp) after the sub and is
    // forwarded in rsi so the helper can dispatch on the call site. All
    // far jumps are absolute via r11: emit_raw code cannot know its own
    // address.
    const uint8_t pro[] = {0x48, 0x83, 0xEC, 0x18,          // sub $0x18,%rsp
                           0x48, 0x89, 0x7C, 0x24, 0x10,    // mov %rdi,0x10(%rsp)
                           0x48, 0x8B, 0x74, 0x24, 0x18};   // mov 0x18(%rsp),%rsi
    put(pro, sizeof(pro));                           // 00..0D
    mov_r11(helper);                                 // 0E
    const uint8_t call_r11[] = {0x41, 0xFF, 0xD3};
    put(call_r11, 3);                                // 18
    const uint8_t test_al[] = {0x84, 0xC0};
    put(test_al, 2);                                 // 1B
    const uint8_t rest[] = {0x48, 0x8B, 0x7C, 0x24, 0x10, // mov 0x10(%rsp),%rdi
                            0x48, 0x8D, 0x64, 0x24, 0x18};// lea 0x18(%rsp),%rsp
    put(rest, sizeof(rest));                         // 1D..26
    const uint8_t jne[] = {0x75, 0x13};
    put(jne, 2);                                     // 27 -> handled 0x3B
    put(kOrigClean, kLenClean);                      // 29..2E (replay)
    mov_r11(resume);                                 // 2F
    const uint8_t jmp_r11[] = {0x41, 0xFF, 0xE3};
    put(jmp_r11, 3);                                 // 39
    // handled (0x3B): exit through the function's own ret
    mov_r11(fnret);                                  // 3B
    put(jmp_r11, 3);                                 // 45
    return k;
}


// ---- read boundary: CSaveHeaderInfo::ReadFileHeader exit --------------------
//
// CSavegameItem::SetupFromFileHeader (0x1d2bfdc) calls
// CSaveHeaderInfo::ReadFileHeader(CSaveGameLoader&) @0x173f44e to pull
// the save's title strings out of the (UTF-8) meta before every list /
// tooltip / continue display. Hooking the read exit and converting the
// info's text fields UTF-8 -> escaped fixes all display consumers at
// once. Installed in a second pass once the field offsets are verified
// live (see install_header_fix).

bool install()
{
    if (std::getenv("EU4CJK_SAVE0")) {
        log_line("[eu4cjk] savefix: disabled (EU4CJK_SAVE0)\n");
        return true;
    }
    bool ok = install_savename_fix() && install_header_fix()
        && install_title_fix()
        && install_rowname_fix(kSiteRowA, kOrigRowA, kResumeRowA,
                               kMovRdiA, kMovRsiA, "rowname-A")
        && install_rowname_fix(kSiteRowB, kOrigRowB, kResumeRowB,
                               kMovRdiB, kMovRsiB, "rowname-B");
    log_line("[eu4cjk] savefix: %s (utf8=0 to_escaped=0 protected=0)\n",
             ok ? "installed" : "FAILED");
    return ok;
}

void log_stats()
{
    log_line("[eu4cjk] savefix final: names_to_utf8=%llu names_to_escaped=%llu"
             " names_protected=%llu names_untouched=%llu"
             " headers_to_escaped=%llu titles_to_escaped=%llu"
             " rownames_to_escaped=%llu\n",
             static_cast<unsigned long long>(g_name_to_utf8.load()),
             static_cast<unsigned long long>(g_name_to_escaped.load()),
             static_cast<unsigned long long>(g_name_protected.load()),
             static_cast<unsigned long long>(g_name_untouched.load()),
             static_cast<unsigned long long>(g_header_to_escaped.load()),
             static_cast<unsigned long long>(g_title_to_escaped.load()),
             static_cast<unsigned long long>(g_rowname_to_escaped.load()));
}

} // namespace eu4cjk::savefix
