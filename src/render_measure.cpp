// M3-T4 measurement family: GetWidth/GetHeight/GetActualRequiredSize/
// GetRequired/GetActRealReq main loops (M-001..M-005) - escape-aware decode
// fixes the wrap width so the engine CJK line breaking activates.
// (Split of the former render.cpp monolith, 2026-09-28.)
#include "render_internal.h"

#include "font.h"
#include "log.h"
#include "stubgen.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace eu4cjk::render {

// ---- M3-T4 measurement family ------------------------------------------------
//
// The wrap/size decision path: GetWidthOfString / GetHeightOfString /
// GetActualRequiredSize / GetRequiredSize / GetActualRealRequiredSizeActually
// all read glyphs byte-wise from the SAME embedded 0x100 table; without decode
// they measure an escaped CJK char as 3 latin advances, so (a) accumulated
// width is wrong (map nameplates scale-to-fit shrink; spacing drifts) and
// (b) the width-overflow branch in the wrap loops (ucomiss vs the box width,
// e.g. 0x20572e6 in ARRA) never fires -> no line breaks. Decoding here lets
// the engine's own wrap machinery work: overflow without a space since the
// last break hits the hard-break path (break between chars), and the +2
// cursor advance keeps every recorded break index on a char boundary.
//
// GetWidthOfString(char* s, int len, bool): font rdi (live after: mulss
// [rdi+0x940]), s rbx, index r14d (r15 = sext copy, set at loop head), byte
// zero-extended into rsi, node -> rbp. After the site the NULL path checks
// sil=='\n', so rsi must hold the byte again (tail). Loop bottom incs r14d.
//   2054027: 48 8b ac f7 00 01 00 00   mov rbp,[rdi+rsi*8+0x100]
constexpr uintptr_t kSiteGW = 0x2054027;
constexpr size_t kLenGW = 8;
constexpr uint8_t kOrigGW[kLenGW] = {
    0x48, 0x8B, 0xAC, 0xF7, 0x00, 0x01, 0x00, 0x00,
};
constexpr uintptr_t kResumeGW = 0x205402f;

// GetHeightOfString(const CString&, ...): CString r14, index ebp (data reg,
// NOT a frame pointer: prologue pushes rbp then reuses it; bottom incs ebp),
// font rbx, node -> rax. Per-char CString::operator[](ebp) hands &s[i] in
// rax right before the fetch:
//   2053d53: 0f b6 00                      movzbl (%rax),%eax
//   2053d56: 48 8b 84 c3 00 01 00 00       mov rax,[rbx+rax*8+0x100]
constexpr uintptr_t kSiteGH = 0x2053d53;
constexpr size_t kLenGH = 11;
constexpr uint8_t kOrigGH[kLenGH] = {
    0x0F, 0xB6, 0x00,
    0x48, 0x8B, 0x84, 0xC3, 0x00, 0x01, 0x00, 0x00,
};
constexpr uintptr_t kResumeGH = 0x2053d5e;

// GetActualRequiredSize(...): CString rbx, index r13d (register; loop bottom
// incs), font at [rsp+0x28], node -> rbp (rbp is a data reg here as well).
//   2056198: 0f b6 00                      movzbl (%rax),%eax
//   205619b: 48 8b 4c 24 28                mov rcx,[rsp+0x28]
//   20561a0: 48 8b ac c1 00 01 00 00       mov rbp,[rcx+rax*8+0x100]
constexpr uintptr_t kSiteGA = 0x2056198;
constexpr size_t kLenGA = 16;
constexpr uint8_t kOrigGA[kLenGA] = {
    0x0F, 0xB6, 0x00,
    0x48, 0x8B, 0x4C, 0x24, 0x28,
    0x48, 0x8B, 0xAC, 0xC1, 0x00, 0x01, 0x00, 0x00,
};
constexpr uintptr_t kResumeGA = 0x20561a8;

// GetRequiredSize main loop: CString r15, index dword [rsp+0x4] (memory
// slot, inc'd at loop top via mov esi,[rsp]; inc esi; mov [rsp],esi), font
// r13, node -> rbp:
//   2056872: 0f b6 00                      movzbl (%rax),%eax
//   2056875: 49 8b ac c5 00 01 00 00       mov rbp,[r13+rax*8+0x100]
// (The 4-iteration preamble reading the global buffer 0x271246c at
// 0x205664f is skipped: head-width only, negligible for escapes.)
constexpr uintptr_t kSiteGR = 0x2056872;
constexpr size_t kLenGR = 11;
constexpr uint8_t kOrigGR[kLenGR] = {
    0x0F, 0xB6, 0x00,
    0x49, 0x8B, 0xAC, 0xC5, 0x00, 0x01, 0x00, 0x00,
};
constexpr uintptr_t kResumeGR = 0x205687d;

// GetActualRealRequiredSizeActually main loop: same shape, CString r12,
// index dword [rsp] (memory slot, same inc pattern at 0x2056ff3), font r15,
// node -> rbp. This is the wrap decision home: width overflow at 0x20572e6
// -> break at last space [rsp+0x38] (word wrap) or hard break at 0x205735a;
// per-char source index table cursor at [rsp+0x130].
// (4-iteration preamble at 0x2056df7 likewise skipped.)
constexpr uintptr_t kSiteGAR = 0x20570b6;
constexpr size_t kLenGAR = 11;
constexpr uint8_t kOrigGAR[kLenGAR] = {
    0x0F, 0xB6, 0x00,
    0x49, 0x8B, 0xAC, 0xC7, 0x00, 0x01, 0x00, 0x00,
};
constexpr uintptr_t kResumeGAR = 0x20570c1;

// ---- M3-T4 measurement installers -------------------------------------------

bool install_measure_common(const char* name, uintptr_t site, size_t len,
                            const uint8_t* orig, uintptr_t resume,
                            const stubgen::StubSpec& spec_tmpl)
{
    stubgen::StubSpec spec = spec_tmpl;
    if (!spec.fn)
        spec.fn = reinterpret_cast<uintptr_t>(&eu4cjk_render_fetch);
    spec.args[2].kind = stubgen::StubSpec::Arg::FrameSlotLea;
    spec.args[2].frame_off = kConsumedSlot;
    spec.args[3].kind = stubgen::StubSpec::Arg::Imm32;   // acc = nullptr:
    spec.args[3].imm = 0;                                // measure loops rebuild
    spec.resume = resume;                                // nothing via us
    uintptr_t stub = stubgen::emit(spec);
    if (!stub || !stubgen::install_jmp(site, orig, len, stub)) {
        log_line("[eu4cjk] measure %s: install failed\n", name);
        return false;
    }
    log_line("[eu4cjk] measure %s: fetch stub @ 0x%lx (site 0x%lx)\n",
             name, static_cast<unsigned long>(stub),
             static_cast<unsigned long>(site));
    return true;
}
bool install_measure_gw(const char* name)
{
    stubgen::StubSpec s;
    // font lives in rdi (caller-saved): the helper call destroys it, but the
    // resume path uses it directly (mulss [rdi+0x940] at 0x205403f; only the
    // kerning path reloads it from [rsp+0x8]). The bool arg in cl is in the
    // same boat (loop-head `test cl,cl` reads it live). Save/restore both.
    s.saves[0].reg = stubgen::Reg::Rdi;
    s.saves[0].restore = true;
    s.saves[1].reg = stubgen::Reg::Rcx;
    s.saves[1].restore = true;
    s.n_saves = 2;
    s.args[0].kind = stubgen::StubSpec::Arg::RegSrc;
    s.args[0].src = stubgen::Reg::Rdi;        // font (arg0 keeps rdi = font)
    s.args[1].kind = stubgen::StubSpec::Arg::IdxSextSum;
    s.args[1].idx = stubgen::Reg::R15;        // p = rbx + sext(r15d)
    s.args[1].base = stubgen::Reg::Rbx;
    s.args[1].has_base = true;
    s.args[4].kind = stubgen::StubSpec::Arg::Imm32;
    s.args[4].imm = 10;                       // site id GW
    s.has_result = true;
    s.result_reg = stubgen::Reg::R11;
    // tail: rsi = byte again (the NULL path tests sil == '\n'); rbx/r15 are
    // callee-saved and survive the helper call. REX 0x42 = REX.X (r15 index).
    static const uint8_t tail[] = {
        0x42, 0x0F, 0xB6, 0x34, 0x3B,         // movzx esi,BYTE PTR [rbx+r15*1]
    };
    s.tail = tail;
    s.tail_len = sizeof(tail);
    // post_tail: rbp = node (data register in GetWidthOfString)
    static const uint8_t post[] = { 0x4C, 0x89, 0xDD }; // mov rbp,r11
    s.post_tail = post;
    s.post_tail_len = sizeof(post);
    s.has_advance = true;
    s.advance_frame_off = kConsumedSlot;
    s.advance_reg = stubgen::Reg::R14;
    return install_measure_common(name, kSiteGW, kLenGW, kOrigGW, kResumeGW, s);
}

bool install_measure_gh(const char* name)
{
    // Index lives in ebp; the engine rbp sits in the stub's pushed slot
    // ([rbp+0] of the anchor), so the conditional +2 targets it directly
    // and the teardown's `pop rbp` restores index+2. Node stays in rax
    // (helper return, original destination).
    stubgen::StubSpec s;
    s.args[0].kind = stubgen::StubSpec::Arg::RegSrc;
    s.args[0].src = stubgen::Reg::Rbx;        // font
    s.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
    s.args[1].src = stubgen::Reg::Rax;        // &s[i] (operator[] result)
    s.args[4].kind = stubgen::StubSpec::Arg::Imm32;
    s.args[4].imm = 11;                       // site id GH
    s.has_advance_mem = true;
    s.advance_frame_off = kConsumedSlot;
    s.advance_mem_rbp_off = 0;                // [rbp+0] = pushed engine rbp
    return install_measure_common(name, kSiteGH, kLenGH, kOrigGH, kResumeGH, s);
}

bool install_measure_ga(const char* name)
{
    stubgen::StubSpec s;
    s.args[0].kind = stubgen::StubSpec::Arg::RbpSlot;
    s.args[0].rbp_off = 0x30;                 // [entry_rsp+0x28] = font
    s.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
    s.args[1].src = stubgen::Reg::Rax;        // &s[i]
    s.args[4].kind = stubgen::StubSpec::Arg::Imm32;
    s.args[4].imm = 12;                       // site id GA
    s.has_result = true;
    s.result_reg = stubgen::Reg::R11;
    // tail: rcx = font (register effect of the replaced mov)
    static const uint8_t tail[] = {
        0x48, 0x8B, 0x4D, 0x30,               // mov rcx,[rbp+0x30]
    };
    s.tail = tail;
    s.tail_len = sizeof(tail);
    static const uint8_t post[] = { 0x4C, 0x89, 0xDD }; // mov rbp,r11
    s.post_tail = post;
    s.post_tail_len = sizeof(post);
    s.has_advance = true;
    s.advance_frame_off = kConsumedSlot;
    s.advance_reg = stubgen::Reg::R13;        // index r13d (loop bottom incs)
    return install_measure_common(name, kSiteGA, kLenGA, kOrigGA, kResumeGA, s);
}

bool install_measure_gr(const char* name)
{
    stubgen::StubSpec s;
    s.args[0].kind = stubgen::StubSpec::Arg::RegSrc;
    s.args[0].src = stubgen::Reg::R13;        // font
    s.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
    s.args[1].src = stubgen::Reg::Rax;        // &s[i]
    s.args[4].kind = stubgen::StubSpec::Arg::Imm32;
    s.args[4].imm = 13;                       // site id GR
    s.has_result = true;
    s.result_reg = stubgen::Reg::R11;
    static const uint8_t post[] = { 0x4C, 0x89, 0xDD }; // mov rbp,r11
    s.post_tail = post;
    s.post_tail_len = sizeof(post);
    s.has_advance_mem = true;
    s.advance_frame_off = kConsumedSlot;
    s.advance_mem_rbp_off = 0xC;              // [entry_rsp+0x4] index slot
    return install_measure_common(name, kSiteGR, kLenGR, kOrigGR, kResumeGR, s);
}

bool install_measure_gar(const char* name)
{
    stubgen::StubSpec s;
    s.args[0].kind = stubgen::StubSpec::Arg::RegSrc;
    s.args[0].src = stubgen::Reg::R15;        // font
    s.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
    s.args[1].src = stubgen::Reg::Rax;        // &s[i]
    s.args[4].kind = stubgen::StubSpec::Arg::Imm32;
    s.args[4].imm = 14;                       // site id GAR
    s.has_result = true;
    s.result_reg = stubgen::Reg::R11;
    static const uint8_t post[] = { 0x4C, 0x89, 0xDD }; // mov rbp,r11
    s.post_tail = post;
    s.post_tail_len = sizeof(post);
    s.has_advance_mem = true;
    s.advance_frame_off = kConsumedSlot;
    s.advance_mem_rbp_off = 0x8;              // [entry_rsp+0x0] index slot
    return install_measure_common(name, kSiteGAR, kLenGAR, kOrigGAR, kResumeGAR, s);
}
} // namespace
