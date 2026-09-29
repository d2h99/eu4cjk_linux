// Render hook orchestration: the install() chain (mode/env gating, per
// family installers, final stats). Family implementations live in
// render_fetch/trace/measure/mapfix/curve.cpp; shared internals in
// render_internal.h. Split of the former monolith, 2026-09-28.
#include "render.h"
#include "render_internal.h"
#include "log.h"
#include "wrapfix.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace eu4cjk::render {

// ---- fetch-site table (FillVB / R2T / R3D / map-name counters) --------------
// CBitmapFont::FillVertexBuffer, char-fetch loop A (index in r14d):
//   2055562: 0f b6 00              movzbl (%rax),%eax
//   2055565: 48 8b 4c 24 68        mov 0x68(%rsp),%rcx        <- font, LIVE after
//   205556a: 4c 8b bc c1 00 01 00 00   mov 0x100(%rcx,%rax,8),%r15
// Register effects to replicate in full mode: rax = byte, rcx = font,
// r15 = glyph node. r14d advances +2 on escape.
constexpr uintptr_t kSiteA = 0x2055562;
constexpr size_t kLenA = 16;
constexpr uint8_t kOrigA[kLenA] = {
    0x0F, 0xB6, 0x00,
    0x48, 0x8B, 0x4C, 0x24, 0x68,
    0x4C, 0x8B, 0xBC, 0xC1, 0x00, 0x01, 0x00, 0x00,
};
constexpr uintptr_t kResumeA = 0x2055572;

// char-fetch loop B (index in ebx, font in r12, result into rax):
//   2055ac1: 0f b6 00              movzbl (%rax),%eax
//   2055ac4: 49 8b 84 c4 00 01 00 00   mov 0x100(%r12,%rax,8),%rax
constexpr uintptr_t kSiteB = 0x2055ac1;
constexpr size_t kLenB = 11;
constexpr uint8_t kOrigB[kLenB] = {
    0x0F, 0xB6, 0x00,
    0x49, 0x8B, 0x84, 0xC4, 0x00, 0x01, 0x00, 0x00,
};
constexpr uintptr_t kResumeB = 0x2055acc;

// CBitmapFont::RenderToTexture, wrap/measure loop (head 0x204ea0e, index in
// r14d, source string at [rsp+0xc0], font at [rsp+0x38], wrap accumulator
// CString at [rsp+0x58]; the engine appends str[i] at the loop top before
// the fetch, so escapes need the payload appended here too - the FillVB-A
// transplant):
//   204ef51: 0f b6 00                 movzbl (%rax),%eax
//   204ef54: 48 8b 4c 24 38           mov 0x38(%rsp),%rcx
//   204ef59: 48 8b ac c1 00 01 00 00  mov 0x100(%rcx,%rax,8),%rbp
// Register effects to replicate: rcx = font, rax = byte, rbp = node (the
// destination is rbp, so the move happens post-frame via post_tail).
constexpr uintptr_t kSiteR2T1 = 0x204ef51;
constexpr size_t kLenR2T1 = 16;
constexpr uint8_t kOrigR2T1[kLenR2T1] = {
    0x0F, 0xB6, 0x00,
    0x48, 0x8B, 0x4C, 0x24, 0x38,
    0x48, 0x8B, 0xAC, 0xC1, 0x00, 0x01, 0x00, 0x00,
};
constexpr uintptr_t kResumeR2T1 = 0x204ef61;

// RenderToTexture emit loop (head 0x2050c1d with section-sign color-code
// scan, index in r14d, rebuilt string in r12, font in r13):
//   2051085: 0f b6 00                 movzbl (%rax),%eax
//   2051088: 4d 8b 9c c5 00 01 00 00  mov 0x100(%r13,%rax,8),%r11
constexpr uintptr_t kSiteR2T2 = 0x2051085;
constexpr size_t kLenR2T2 = 11;
constexpr uint8_t kOrigR2T2[kLenR2T2] = {
    0x0F, 0xB6, 0x00,
    0x4D, 0x8B, 0x9C, 0xC5, 0x00, 0x01, 0x00, 0x00,
};
constexpr uintptr_t kResumeR2T2 = 0x2051090;

// CBitmapFont::Render3d (font = this in r12 from the prologue), wrap/measure
// loop (head 0x2054553, index in r15d, source via rbp, wrap accumulator
// CString at [rsp+0x58], '_'/'~'/section-sign handling around the fetch):
//   20546d4: 0f b6 00                 movzbl (%rax),%eax
//   20546d7: 49 8b 9c c4 00 01 00 00  mov 0x100(%r12,%rax,8),%rbx
constexpr uintptr_t kSiteR3D1 = 0x20546d4;
constexpr size_t kLenR3D1 = 11;
constexpr uint8_t kOrigR3D1[kLenR3D1] = {
    0x0F, 0xB6, 0x00,
    0x49, 0x8B, 0x9C, 0xC4, 0x00, 0x01, 0x00, 0x00,
};
constexpr uintptr_t kResumeR3D1 = 0x20546df;

// Render3d emit loop (rebuilt string in r13, index in r15d, font in r12):
//   2054ccd: 0f b6 00                 movzbl (%rax),%eax
//   2054cd0: 49 8b 84 c4 00 01 00 00  mov 0x100(%r12,%rax,8),%rax
constexpr uintptr_t kSiteR3D2 = 0x2054ccd;
constexpr size_t kLenR3D2 = 11;
constexpr uint8_t kOrigR3D2[kLenR3D2] = {
    0x0F, 0xB6, 0x00,
    0x49, 0x8B, 0x84, 0xC4, 0x00, 0x01, 0x00, 0x00,
};
constexpr uintptr_t kResumeR3D2 = 0x2054cd8;

// Map-name sizing loops. CCountryNameCollection::AddNudgedNames and
// CGenerateNamesWork::AddNameArea count "visible" glyphs with their OWN
// inline-table reads (NULL or height<=0 skips) and alloca(count*120) a
// vertex buffer afterwards. With escape-aware FillVertexBuffer emitting
// CJK glyphs the vanilla count is too small and the buffer overflows into
// the caller's frame (the "Ming crash", 2026-09-27 03:2x). Hooking these
// two reads with the same escape decode keeps count == emitted glyphs.
// Both sites: font r12, node -> rax, loop index ebx (inc at the tail).
//   1b5e213: 0f b6 00                 movzbl (%rax),%eax
//   1b5e216: 49 8b 84 c4 00 01 00 00  mov 0x100(%r12,%rax,8),%rax
constexpr uintptr_t kSiteCnt1 = 0x1b5e213;
constexpr size_t kLenCnt1 = 11;
constexpr uint8_t kOrigCnt1[kLenCnt1] = {
    0x0F, 0xB6, 0x00,
    0x49, 0x8B, 0x84, 0xC4, 0x00, 0x01, 0x00, 0x00,
};
constexpr uintptr_t kResumeCnt1 = 0x1b5e21e;

//   1b5fe41: 0f b6 00                 movzbl (%rax),%eax
//   1b5fe44: 49 8b 84 c4 00 01 00 00  mov 0x100(%r12,%rax,8),%rax
constexpr uintptr_t kSiteCnt2 = 0x1b5fe41;
constexpr size_t kLenCnt2 = 11;
constexpr uint8_t kOrigCnt2[kLenCnt2] = {
    0x0F, 0xB6, 0x00,                         // movzbl (%rax),%eax
    0x49, 0x8B, 0x84, 0xC4, 0x00, 0x01, 0x00, 0x00, // mov 0x100(%r12,%rax,8),%rax
};
constexpr uintptr_t kResumeCnt2 = 0x1b5fe4c;

bool install()
{
    const char* m = std::getenv("EU4CJK_RENDER");
    if (m && std::strcmp(m, "probe") == 0) g_mode = mProbe;
    else if (m && std::strcmp(m, "half") == 0) g_mode = mHalf;
    else if (m && std::strcmp(m, "r2t") == 0) g_mode = mR2TOnly;
    else g_mode = mFull;
    const char* mode_name = g_mode == mProbe ? "probe"
                          : g_mode == mHalf ? "half"
                          : g_mode == mR2TOnly ? "r2t-only" : "full";
    // 3D-label decode: default ON in full mode now that the map-font texture
    // loads (half-res repack under the engine's 16MiB file cap) and CurveText
    // is escape-aware; EU4CJK_3D=0 disables (vanilla 3D labels, bisect aid).
    const char* e3d = std::getenv("EU4CJK_3D");
    g_3d_decode = g_mode == mFull && !(e3d && std::strcmp(e3d, "0") == 0);
    g_diag = std::getenv("EU4CJK_DIAG") != nullptr;
    // M3-T4 measurement family: on in full mode; EU4CJK_MEASURE=0 disables
    // (bisection switch if a measure site misbehaves).
    const char* meas = std::getenv("EU4CJK_MEASURE");
    bool measure_on = !(meas && std::strcmp(meas, "0") == 0)
                      && g_mode == mFull;

    bool ok;
    if (g_mode == mProbe) {
        ok = install_probe("FillVB-A", 0, kSiteA, kLenA, kOrigA, kResumeA)
          && install_probe("FillVB-B", 1, kSiteB, kLenB, kOrigB, kResumeB);
    } else if (g_mode == mHalf) {
        ok = install_fetch("FillVB-A", kSiteA, kLenA, kOrigA, kResumeA, true)
          && install_fetch("FillVB-B", kSiteB, kLenB, kOrigB, kResumeB, false);
    } else if (g_mode == mR2TOnly) {
        // bisect: FillVB/CNT/R3D off (no 3D-quad emission), R2T keeps
        // tooltips/popups decoding.
        ok = install_fetch_r2t("R2T-1", kSiteR2T1, kLenR2T1, kOrigR2T1,
                               kResumeR2T1, true)
          && install_fetch_r2t("R2T-2", kSiteR2T2, kLenR2T2, kOrigR2T2,
                               kResumeR2T2, false);
    } else {
        ok = install_fetch("FillVB-A", kSiteA, kLenA, kOrigA, kResumeA, true)
          && install_fetch("FillVB-B", kSiteB, kLenB, kOrigB, kResumeB, false)
          && install_fetch_r2t("R2T-1", kSiteR2T1, kLenR2T1, kOrigR2T1,
                               kResumeR2T1, true)
          && install_fetch_r2t("R2T-2", kSiteR2T2, kLenR2T2, kOrigR2T2,
                               kResumeR2T2, false)
          && install_fetch_r3d("R3D-1", kSiteR3D1, kLenR3D1, kOrigR3D1,
                               kResumeR3D1, true)
          && install_fetch_r3d("R3D-2", kSiteR3D2, kLenR3D2, kOrigR3D2,
                                kResumeR3D2, false)
          && install_fetch_rts1("RTS-1")
          && install_fetch_rts2("RTS-2")
          // FVB-entry caller recorder: its only consumer
          // (fvb_caller_is_3d_label) is dead while 3D decode is on, so
          // install only for the EU4CJK_3D=0 bisect config (release
          // default skips: one less hot-path patch point).
          && (g_3d_decode ? true : install_fvb_entry());
        if (ok && !std::getenv("EU4CJK_CASE0")) {
            // toupper/tolower PLT targets verified against the 11B site bytes.
            static const uint8_t up_orig[10] = {
                0x0F, 0xBE, 0xF8, 0xE8, 0x18, 0x64, 0x7F, 0xFE, 0x88, 0x03};
            static const uint8_t lo_orig[10] = {
                0x0F, 0xBE, 0xF8, 0xE8, 0xE4, 0x88, 0x7F, 0xFE, 0x88, 0x03};
            ok = install_case_fix("ToUpper", 0x254cf20, up_orig, 0x254cf2a, 0xd43340)
              && install_case_fix("ToLower", 0x254ced4, lo_orig, 0x254cede, 0xd457c0);
            if (ok && !std::getenv("EU4CJK_SPLIT0"))
                ok = install_split_fix();
            if (ok && g_diag) {
                // non-fatal: diagnostics only
                install_case_probe_at("ToUpper", 0x254cefe, 0x254cf05,
                                      &eu4cjk_case_probe);
                install_case_probe_at("ToLower", 0x254ceb2, 0x254ceb9,
                                      &eu4cjk_case_probe_lo);
            }
        }
        if (ok && g_3d_decode)
            ok = install_fetch_count("CNT-1", kSiteCnt1, kLenCnt1, kOrigCnt1,
                                     kResumeCnt1, 8)
              && install_fetch_count("CNT-2", kSiteCnt2, kLenCnt2, kOrigCnt2,
                                     kResumeCnt2, 9)
              && (std::getenv("EU4CJK_CT0")
                      ? true
                      : install_measure_ct("CurveText") && install_ct_tdiv_fix()
                            && install_ct_single_fix());
        if (g_3d_decode)
            log_line("[eu4cjk] render: 3D-label decode on\n");
        // GW (GetWidthOfString) x 3D-decode = deterministic _exit(11) crash:
        // the engine's len slot [rsp+0x10] gets corrupted during GW-hooked
        // measuring (fault ctx: r14 index runaway to ~1e7, rdx=stack garbage)
        // - stable with GW off (3/3) or 3D off (legacy). GW serves UI wrap
        // width only; map labels take priority - keep GW off while 3D decode
        // is on (EU4CJK_GW0=1 forces off in any config).
        const bool gw_off = g_3d_decode || std::getenv("EU4CJK_GW0");
        if (ok && measure_on)
            ok = (gw_off ? true : install_measure_gw("GetWidth"))
              && install_measure_gh("GetHeight")
              && install_measure_ga("GetActualReq")
              && install_measure_gr("GetRequired")
              && install_measure_gar("GetActRealReq");
        else if (measure_on)
            log_line("[eu4cjk] measure: skipped (render install failed)\n");
        if (ok && gw_off)
            log_line("[eu4cjk] measure GW: disabled (3D-decode interaction)\n");
        // M4-P1d wrap-fix: CJK chars must become line-break candidates in the
        // engine's inline wrap gates (upstream mainTextProc3 equivalent).
        if (ok)
            ok = wrapfix::install();
    }

    log_line("[eu4cjk] render: mode=%s %s\n", mode_name, ok ? "installed" : "FAILED");

    install_entry_traces();

    std::atexit([] {
        log_line("[eu4cjk] render final stats: FillVB-A=%llu FillVB-B=%llu"
                 " R2T-1=%llu R2T-2=%llu R3D-1=%llu R3D-2=%llu"
                 " RTS-1=%llu RTS-2=%llu CNT-1=%llu CNT-2=%llu CT-1=%llu"
                 " GW=%llu GH=%llu GA=%llu GR=%llu GAR=%llu"
                 " escapes=%llu payload_appended=%llu"
                 " notecjk0=%llu notecjk2=%llu notecjk4=%llu notecjk6=%llu"
                 " notecjk10=%llu\n",
                 static_cast<unsigned long long>(g_site_hits[0].load()),
                 static_cast<unsigned long long>(g_site_hits[1].load()),
                 static_cast<unsigned long long>(g_site_hits[2].load()),
                 static_cast<unsigned long long>(g_site_hits[3].load()),
                 static_cast<unsigned long long>(g_site_hits[4].load()),
                 static_cast<unsigned long long>(g_site_hits[5].load()),
                 static_cast<unsigned long long>(g_site_hits[6].load()),
                 static_cast<unsigned long long>(g_site_hits[7].load()),
                 static_cast<unsigned long long>(g_site_hits[8].load()),
                 static_cast<unsigned long long>(g_site_hits[9].load()),
                 static_cast<unsigned long long>(g_site_hits[15].load()),
                 static_cast<unsigned long long>(g_site_hits[10].load()),
                 static_cast<unsigned long long>(g_site_hits[11].load()),
                 static_cast<unsigned long long>(g_site_hits[12].load()),
                 static_cast<unsigned long long>(g_site_hits[13].load()),
                 static_cast<unsigned long long>(g_site_hits[14].load()),
                 static_cast<unsigned long long>(g_escapes.load()),
                 static_cast<unsigned long long>(g_appends.load()),
                 static_cast<unsigned long long>(wrapfix::note_cjk_count(0)),
                 static_cast<unsigned long long>(wrapfix::note_cjk_count(2)),
                 static_cast<unsigned long long>(wrapfix::note_cjk_count(4)),
                 static_cast<unsigned long long>(wrapfix::note_cjk_count(6)),
                 static_cast<unsigned long long>(wrapfix::note_cjk_count(10)));
        log_caller_histogram();
        log_trace_final();
    });
    return ok;
}
}
