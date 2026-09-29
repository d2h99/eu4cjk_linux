// CurveText family (M4-P2): CT-1 escape-aware fetch (charset-0x100 font),
// the curve-parameter t-div fix (+2 cursor compensation) and the
// single-glyph sampler flat-baseline fix (P2e). build_ct_* stubs are shared
// with the offline selftest (declared in render.h). (Split of the former
// render.cpp monolith, 2026-09-28.)
#include "render.h"
#include "render_internal.h"
#include "log.h"
#include "stubgen.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <cstring>

namespace eu4cjk::render {

// CurveText (0x1b5eac7, sole caller AddNameArea 0x1b5e349) bends the
// FillVB-emitted glyph vertices onto the map curve. Its char loop reads
// bytes one at a time (CString::operator[]) from the 256-entry inline
// table: r12 = CBitmapCharacterSet* = font + 0x100 (pushed by the caller;
// charset+0x840 = font+0x940 = scale). Escapes must decode to ONE glyph
// and advance the index r14d by 3 (loop bottom `inc r14d` at 0x1b5fbb7;
// the body path saves r14d at 0x1b5f88f and reloads it at 0x1b5fbb0, so
// the in-register +2 advance survives both paths; the NULL/h<=0 skip
// paths jump straight to the inc, which is why the advance MUST live in
// this stub - moving it to the loop bottom desyncs the skip paths).
// Bound at [rbp-0xd0] = GetSize() in BYTES, so +3/escape lands exactly
// (jne terminator). node -> r13, ZF for the NULL-skip je at 0x1b5f474
// replicated via post_tail.
// The in-stub advance skews the curve parameter t = r14d/(size-1) (div at
// 0x1b5f567) by +2 - see install_ct_tdiv_fix.
//   1b5f463: 0f b6 00           movzbl (%rax),%eax
//   1b5f466: 4d 8b 2c c4        mov (%r12,%rax,8),%r13
//   1b5f46a: 4d 85 ed           test %r13,%r13
constexpr uintptr_t kSiteCT1 = 0x1b5f463;
constexpr size_t kLenCT1 = 10;
constexpr uint8_t kOrigCT1[kLenCT1] = {
    0x0F, 0xB6, 0x00,
    0x4D, 0x8B, 0x2C, 0xC4,
    0x4D, 0x85, 0xED,
};
constexpr uintptr_t kResumeCT1 = 0x1b5f46d;

// CT-1 helper: the site holds the character-set pointer (font + 0x100) in
// r12, while eu4cjk_render_fetch expects the font object itself.
extern "C" const void* eu4cjk_ct_fetch(const void* charset, const char* p,
                                       uint32_t* consumed, void* acc,
                                       uint32_t site, uintptr_t copy_idx)
{
    return eu4cjk_render_fetch(static_cast<const uint8_t*>(charset) - 0x100,
                               p, consumed, acc, site, copy_idx);
}
// CurveText char loop (map-name curve layout). All engine volatiles are dead
// at the site (it directly follows the CString::operator[] call), and the
// helper preserves the callee-saved rbx/rbp/r12/r14/r15 - no saves needed.
bool install_measure_ct(const char* name)
{
    stubgen::StubSpec s;
    s.fn = reinterpret_cast<uintptr_t>(&eu4cjk_ct_fetch);
    s.args[0].kind = stubgen::StubSpec::Arg::RegSrc;
    s.args[0].src = stubgen::Reg::R12;        // charset = font + 0x100
    s.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
    s.args[1].src = stubgen::Reg::Rax;        // &s[i] (operator[] result)
    s.args[4].kind = stubgen::StubSpec::Arg::Imm32;
    s.args[4].imm = 15;                       // site id CT-1
    s.has_result = true;
    s.result_reg = stubgen::Reg::R13;         // node -> r13
    // post_tail runs last (after the advance's cmp): re-establish ZF for the
    // engine's NULL-skip je at 0x1b5f474.
    static const uint8_t post[] = { 0x4D, 0x85, 0xED }; // test r13,r13
    s.post_tail = post;
    s.post_tail_len = sizeof(post);
    s.has_advance = true;
    s.advance_frame_off = kConsumedSlot;
    s.advance_reg = stubgen::Reg::R14;        // char index (loop bottom incs)
    return install_measure_common(name, kSiteCT1, kLenCT1, kOrigCT1, kResumeCT1, s);
}
// CurveText curve-parameter skew fix (M4-P2, 康-at-Persian-Gulf bug):
// the CT-1 stub advances r14d by +2 at fetch time (required - the NULL /
// h<=0 skip paths jump straight to the loop-bottom inc), but the loop's
// curve interpolation parameter is computed AFTER the fetch:
//   1b5f562: cvtsi2ss %r14d,%xmm6      t_num = r14d
//   1b5f567: divss -0x154(%rbp),%xmm6  t = r14d / (GetSize()-1)
// so every escape glyph's t is skewed by +2/(size-1). Single-CJK-char
// names (letter-spaced to "康␣␣␣", size 6) land at t=0.4 instead of 0 -
// i.e. 40% along the country curve: 康 rendered near the Persian Gulf
// instead of Kham. This 5-byte-site stub recomputes the numerator as
// (r14d - 2) when r14d points at the last payload byte of an escape
// (buf[r14d-2] in 0x10..0x13), restoring vanilla t semantics. rax/rcx/r11
// are dead at the site (rcx is reloaded at 0x1b5f744 before any use).
bool install_ct_tdiv_fix()
{
    constexpr uintptr_t kSite = 0x1b5f562;    // cvtsi2ss %r14d,%xmm6
    constexpr uintptr_t kResume = 0x1b5f567;  // divss -0x154(%rbp),%xmm6
    static const uint8_t orig[5] = { 0xF3, 0x41, 0x0F, 0x2A, 0xF6 };

    uint8_t s[64];
    const size_t len = build_ct_tdiv_stub(s, sizeof(s), kResume);
    if (!len) {
        log_line("[eu4cjk] ct-tdiv: builder overflow\n");
        return true;   // degrade to the old visible-but-skewed behaviour
    }
    const uintptr_t stub = stubgen::emit_raw(s, len);
    if (!stub || !stubgen::install_jmp(kSite, orig, sizeof(orig), stub)) {
        log_line("[eu4cjk] ct-tdiv: install failed (stub=%p) - names keep the"
                 " +2 curve skew (flung single-char names)\n",
                 reinterpret_cast<void*>(stub));
        return true;   // degrade to the old visible-but-skewed behaviour
    }
    log_line("[eu4cjk] ct-tdiv: stub @ 0x%lx (site 0x%lx)\n",
             static_cast<unsigned long>(stub), static_cast<unsigned long>(kSite));
    return true;
}
// CurveText t-numerator stub (see install_ct_tdiv_fix): xmm6 = (float)
// (r14d - (buf[r14d-2] in 0x10..0x13 && r14d >= 2 ? 2 : 0)), then resume
// at the divss. Shared by the installer and the offline selftest.
size_t build_ct_tdiv_stub(uint8_t* out, size_t cap, uintptr_t resume)
{
    size_t k = 0;
    auto put = [&](std::initializer_list<uint8_t> bs) {
        for (uint8_t b : bs) out[k++] = b;
    };
    auto mov_r11 = [&](uintptr_t v) {
        put({0x49, 0xBB});
        std::memcpy(out + k, &v, 8);
        k += 8;
    };
    if (cap < 7 + 3 + 3 + 3 + 2 + 5 + 2 + 2 + 2 + 3 + 4 + 10 + 3) return 0;
    put({0x48, 0x8B, 0x85, 0xB8, 0xFE, 0xFF, 0xFF});  // mov -0x148(%rbp),%rax (CString*)
    put({0x48, 0x8B, 0x00});                          // mov (%rax),%rax       buf
    put({0x41, 0x8B, 0xCE});                          // mov %r14d,%ecx
    put({0x83, 0xF9, 0x02});                          // cmp $0x2,%ecx
    put({0x72, 0x10});                                // jb  -> emit (ecx < 2)
    put({0x0F, 0xB6, 0x44, 0x08, 0xFE});              // movzbl -0x2(%rax,%rcx,1),%eax
    put({0x3C, 0x10});                                // cmp $0x10,%al
    put({0x72, 0x07});                                // jb  -> emit
    put({0x3C, 0x13});                                // cmp $0x13,%al
    put({0x77, 0x03});                                // ja  -> emit
    put({0x83, 0xE9, 0x02});                          // sub $0x2,%ecx
    put({0xF3, 0x0F, 0x2A, 0xF1});                    // emit: cvtsi2ss %ecx,%xmm6
    mov_r11(resume);
    put({0x41, 0xFF, 0xE3});                          // jmp *%r11
    return k;
}
// CurveText single-glyph sampler fix (M4-P2b/P2c/P2d, tilted 康 / squished
// 奥斯曼): the pre-loop sampler walks glyph quads (r15 = vertex offset,
// step 6) storing curve samples: the FIRST quad (r15==0, test/je at
// 0x1b5ee44) stores [-0xe0] = midY(first), [-0x70].low = midX(first); the
// LAST quad (r15==n-6, cmp at 0x1b5ee49) stores [-0xd8] = midX(last),
// [-0x70].high = midY(last). The baseline direction for glyph rotation is
// derived from these vs avg (xmm2 = accumulated glyph centers / count):
// dX = avg.x - [-0xd8], dY = avg.y - [-0x70].high; cos = dX/len,
// sin = dY/len (0x1b5eff2). Multi-glyph names: mids bracket the text run
// (MUL: cos ~ -0.96..-0.99 = correct regime, final quads axis-aligned).
// True single-glyph names (康/明/科) break it two ways: (a) n==6 (one
// quad): last-quad branch unreachable -> zero slots -> window collapses ->
// glyph at world origin (invisible; with the old t-skew the same collapse
// produced the historical "康 near the Persian Gulf"); (b) n>=12
// (multi-quad glyph): first/last mids are INTRA-glyph quad midpoints ->
// dX/dY = sub-glyph noise -> random rotation (康 tilted 133 deg).
// Fix: detect TRUE single-glyph names by string shape and synthesize a
// clean flat baseline around avg: [-0x70].low = avg.x - K, [-0xe0] =
// avg.y, [-0xd8] = avg.x + K, [-0x70].high = avg.y (K = 4.9 ~ half a
// glyph advance, from MUL spans ~9.5/glyph) -> cos = -1, sin = 0 -> same
// regime as multi-glyph names -> upright, position centered on avg.
//
// Single-glyph detection (P2d/P2e): a name is single-glyph iff its string
// contains EXACTLY ONE escape marker (0x10..0x13, uniformly 3 bytes each:
// marker + 16-bit payload, see fetch_glyph). The P2c check "byte3 == 0x20"
// was wrong (multi-glyph names also carry a letter-space right after the
// first escape: 奥斯曼 = 10 65 59 20 10 .., 西藏 = 10 7f 89 20 20 10 ..),
// squishing every spaced multi-glyph name into avg +- 4.9. The P2d
// "all bytes 0x20 until NUL" check was also wrong: engine map-label
// strings use an explicit length (CString+8), and the bytes past the end
// are heap noise - a single-glyph name followed by junk ("劳␣" + stale
// "1r..") failed the all-spaces test and kept its tilted intra-glyph
// rotation. Correct rule (P2e): scan ONLY the length-bounded string for
// a second escape: rdx = data+3, rcx = (int)len - 3 (signed); if any
// byte in 0x10..0x13 appears -> multi-glyph -> replay untouched; if the
// scan exhausts -> single-glyph -> apply the flat baseline. Non-escape
// tail bytes (spaces, transform leftovers like '1''r' that emit no
// vertices) do not block the fix. Known limitation: a hypothetical
// "CJK + real ASCII letters" mixed name would be treated as single (its
// letters would squish); no such map label exists in CJK saves.
size_t build_ct_single_stub(uint8_t* out, size_t cap, uintptr_t resume)
{
    size_t k = 0;
    auto put = [&](std::initializer_list<uint8_t> bs) {
        for (uint8_t b : bs) out[k++] = b;
    };
    auto mov_r11 = [&](uintptr_t v) {
        put({0x49, 0xBB});
        std::memcpy(out + k, &v, 8);
        k += 8;
    };
    // layout (offsets): 00 mov rax(7) 07 mov rdx(3) 0A movslq rcx(4)
    //   0E cmpb(3) 11 jb(2) 13 cmpb(3) 16 ja(2) 18 lea(4) 1C sub(4)
    //   20 loop: test(3) 23 jle(2) 25 cmpb(3) 28 jb(2) 2A cmpb(3)
    //   2D jbe(2) 2F next: inc(3) 32 dec(3) 35 jmp(2) 37 apply: movsd K(8)
    //   3F movapd(4) 43 subsd(4) 47 movsd(5) 4C movapd(4) 50 unpckhpd(4)
    //   54 movsd e0(8) 5C movsd 68(5) 61 movapd(4) 65 addsd K(8)
    //   6D movsd d8(8) 75 replay(7) 7C r11(10) 86 jmp(3) 89 K(8) = 145
    if (cap < 145) return 0;
    put({0x48, 0x8B, 0x85, 0xB8, 0xFE, 0xFF, 0xFF});  // mov -0x148(%rbp),%rax
    put({0x48, 0x8B, 0x10});                          // mov (%rax),%rdx
    put({0x48, 0x63, 0x48, 0x08});                    // movslq 0x8(%rax),%rcx
    put({0x80, 0x3A, 0x10});                          // cmpb $0x10,(%rdx)
    put({0x72, 0x62});                                // jb  -> replay
    put({0x80, 0x3A, 0x13});                          // cmpb $0x13,(%rdx)
    put({0x77, 0x5D});                                // ja  -> replay
    put({0x48, 0x8D, 0x52, 0x03});                    // lea 0x3(%rdx),%rdx
    put({0x48, 0x83, 0xE9, 0x03});                    // sub $0x3,%rcx
    put({0x48, 0x85, 0xC9});                          // loop: test %rcx,%rcx
    put({0x7E, 0x12});                                // jle -> apply
    put({0x80, 0x3A, 0x10});                          // cmpb $0x10,(%rdx)
    put({0x72, 0x05});                                // jb  -> next
    put({0x80, 0x3A, 0x13});                          // cmpb $0x13,(%rdx)
    put({0x76, 0x46});                                // jbe -> replay (2nd escape)
    put({0x48, 0xFF, 0xC2});                          // next: inc %rdx
    put({0x48, 0xFF, 0xC9});                          // dec %rcx (/1, not C1=inc!)
    put({0xEB, 0xE9});                                // jmp loop
    put({0xF2, 0x0F, 0x10, 0x0D, 0x4A, 0x00, 0x00, 0x00}); // movsd K(%rip),%xmm1
    put({0x66, 0x0F, 0x28, 0xC2});                    // movapd %xmm2,%xmm0
    put({0xF2, 0x0F, 0x5C, 0xC1});                    // subsd %xmm1,%xmm0
    put({0xF2, 0x0F, 0x11, 0x45, 0x90});              // movsd %xmm0,-0x70(%rbp)
    put({0x66, 0x0F, 0x28, 0xC2});                    // movapd %xmm2,%xmm0
    put({0x66, 0x0F, 0x15, 0xC2});                    // unpckhpd %xmm2,%xmm0
    put({0xF2, 0x0F, 0x11, 0x85, 0x20, 0xFF, 0xFF, 0xFF}); // movsd %xmm0,-0xe0(%rbp)
    put({0xF2, 0x0F, 0x11, 0x45, 0x98});              // movsd %xmm0,-0x68(%rbp)
    put({0x66, 0x0F, 0x28, 0xC2});                    // movapd %xmm2,%xmm0
    put({0xF2, 0x0F, 0x58, 0x05, 0x1C, 0x00, 0x00, 0x00}); // addsd K(%rip),%xmm0
    put({0xF2, 0x0F, 0x11, 0x85, 0x28, 0xFF, 0xFF, 0xFF}); // movsd %xmm0,-0xd8(%rbp)
    put({0x4C, 0x8B, 0xB5, 0x28, 0xFF, 0xFF, 0xFF});  // replay: mov -0xd8(%rbp),%r14
    mov_r11(resume);
    put({0x41, 0xFF, 0xE3});                          // jmp *%r11
    const double kval = 4.9;                          // K: half glyph advance
    std::memcpy(out + k, &kval, 8);
    k += 8;
    return k;
}

bool install_ct_single_fix()
{
    constexpr uintptr_t kSite = 0x1b5ef36;    // mov -0xd8(%rbp),%r14
    constexpr uintptr_t kResume = 0x1b5ef3d;  // movaps -0x70(%rbp),%xmm0
    static const uint8_t orig[7] = { 0x4C, 0x8B, 0xB5, 0x28, 0xFF, 0xFF, 0xFF };

    uint8_t s[160];
    const size_t len = build_ct_single_stub(s, sizeof(s), kResume);
    if (!len) {
        log_line("[eu4cjk] ct-single: builder overflow\n");
        return true;   // degrade: single-char names stay invisible
    }
    const uintptr_t stub = stubgen::emit_raw(s, len);
    if (!stub || !stubgen::install_jmp(kSite, orig, sizeof(orig), stub)) {
        log_line("[eu4cjk] ct-single: install failed (stub=%p)\n",
                 reinterpret_cast<void*>(stub));
        return true;
    }
    log_line("[eu4cjk] ct-single: stub @ 0x%lx (site 0x%lx)\n",
             static_cast<unsigned long>(stub), static_cast<unsigned long>(kSite));
    return true;
}
} // namespace
