// Fetch-family hooks: FillVertexBuffer A/B, RenderToTexture 1/2, Render3d
// 1/2, RenderToScreen 1/2, map-name sizing counters, the FVB-entry caller
// recorder, the shared eu4cjk_render_fetch decode helper and the render
// globals. (Split of the former render.cpp monolith, 2026-09-28.)
#include "render_internal.h"

#include "font.h"
#include "wrapfix.h"
#include "log.h"
#include "stubgen.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace eu4cjk::render {
Mode g_mode = mFull;
// 3D-label decode (default on in full mode; EU4CJK_3D=0 disables): escapes
// decode for the 3D label builders too - CNT count hooks keep their alloca
// in sync with emission and CT-1 makes CurveText escape-aware.
bool g_3d_decode = false;
bool g_diag = false;              // EU4CJK_DIAG: map-font one-shot dumps
std::atomic<uint64_t> g_diag_dumps{0};

std::atomic<uint64_t> g_hits[2];
std::atomic<uint64_t> g_escapes;
std::atomic<uint64_t> g_appends;
std::atomic<uint64_t> g_site_hits[16]; // A,B,R2T-1/2,R3D-1/2,RTS-1/2,CNT-1/2,GW,GH,GA,GR,GAR,CT-1

// ---- FillVB caller awareness ------------------------------------------------
// FillVertexBuffer decodes escaped CJK fine for GUI text (menu buttons show
// hanzi), but the 3D label builders (CGenerateNamesWork::AddNameArea,
// CCountryNameCollection::AddNudgedNames ~0x1b5d000-0x1b62000) feed vertex
// data into the animated map-text path whose draw/animation logic then shows
// garbage (flickering giant triangles on zoom, 2026-09-27). So A/B behave
// vanilla when FillVB was called from that range. The entry hook records the
// caller return address in TLS; a caller histogram is logged at exit.
static constexpr uintptr_t k3DLabelLo = 0x1b5d000;
static constexpr uintptr_t k3DLabelHi = 0x1b62000;
static __thread uintptr_t t_fvb_caller = 0;
struct CallerHit { uintptr_t ret; std::atomic<uint64_t> n; };
static CallerHit g_callers[24];

extern "C" void eu4cjk_fillvb_entry(uintptr_t ret)
{
    t_fvb_caller = ret;
    for (auto& h : g_callers) {
        uintptr_t r = h.ret;
        if (r == ret) { h.n.fetch_add(1, std::memory_order_relaxed); return; }
    }
    for (auto& h : g_callers) {
        uintptr_t expected = 0;
        if (h.ret == 0 && h.n.compare_exchange_strong(expected, 1)) {
            h.ret = ret;
            return;
        }
    }
}

static bool fvb_caller_is_3d_label()
{
    uintptr_t c = t_fvb_caller;
    return c >= k3DLabelLo && c < k3DLabelHi;
}


void log_caller_histogram()
{
    for (auto& h : g_callers) {
        if (h.ret)
            log_line("[eu4cjk] FVB caller 0x%lx x%llu%s\n",
                     static_cast<unsigned long>(h.ret),
                     static_cast<unsigned long long>(h.n.load()),
                     (h.ret >= k3DLabelLo && h.ret < k3DLabelHi)
                         ? " (3D-label: vanilla)" : "");
    }
}

// Engine helpers for the wrap-loop string rebuild (absolute, non-PIE). The
// measure loop appends str[i] at its top via CString(char)+operator+=
// (0x205552b-0x2055557) BEFORE the fetch; our escape consumption skips the
// payload bytes, so they must be appended here or the rebuilt wrap string
// loses them and the emit loop (site B) decodes garbage (the M3-T3
// "invisible map text" finding, 2026-09-27).
// CString layout (from 0x254bf68): [+0]=data (self-referential SSO at
// this+0x10 for short strings), [+8]=size, [+0x10]=inline storage. The
// engine's post-append check "if (temp.data != r12) delete" frees only
// HEAP buffers; r12 is the engine temp's own SSO address, so ours must be
// tmp+0x10 (the live crash 2026-09-27 00:22 was free(stack_ptr)).
constexpr uintptr_t kCStrFromChar = 0x254bf68;   // CString::CString(char)
constexpr uintptr_t kCStrAppendMove = 0x254c184; // CString::operator+=(CString&&)
constexpr uintptr_t kOperatorDelete = 0xd43da0;  // operator delete(void*)

void append_byte(void* acc, char c)
{
    alignas(16) uint8_t tmp[0x40]; // engine's temp CString spans [rsp+0x10,0x38)
    std::memset(tmp, 0, sizeof(tmp));
    reinterpret_cast<void (*)(void*, char)>(kCStrFromChar)(tmp, c);
    reinterpret_cast<void (*)(void*, void*)>(kCStrAppendMove)(acc, tmp);
    void* data = *reinterpret_cast<void**>(tmp);
    if (data != tmp + 0x10)
        reinterpret_cast<void (*)(void*)>(kOperatorDelete)(data);
}
extern "C" void eu4cjk_probe_hit(uint32_t tag, const char* p)
{
    uint64_t n = g_hits[tag & 1].fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1)
        log_line("[eu4cjk] probe[%u]: first hit, byte=0x%02X\n",
                 static_cast<unsigned>(tag),
                 static_cast<unsigned>(static_cast<uint8_t>(*p)));
}
// Stub helper for the fetch sites. rsi points at &s[i] on entry.
// acc (wrap loops only) = the accumulating CString (&engine [rsp+0x58] or
// +0x38 depending on the site). copy_idx (RTS-1 only) = the word-buffer copy
// cursor (r12); the prefix byte was already copied by the loop head, so the
// escape payload goes to word_buf[copy_idx..+1] to keep the strcat-fed copy
// escape-intact for the second (render) loop.
// site: 0=FillVB-A 1=FillVB-B 2=R2T-1 3=R2T-2 4=R3D-1 5=R3D-2 6=RTS-1 7=RTS-2
extern "C" const void* eu4cjk_render_fetch(const void* font, const char* p,
                                            uint32_t* consumed, void* acc,
                                            uint32_t site, uintptr_t copy_idx)
{
    if (site < kSiteCount) {
        uint64_t n = g_site_hits[site].fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1)
            log_line("[eu4cjk] fetch site %u: first hit, byte=0x%02X\n",
                     static_cast<unsigned>(site),
                     static_cast<unsigned>(static_cast<uint8_t>(p[0])));
    }

    // EU4CJK_DIAG: per-site sampler of escape-led strings (dedup: first 10
    // bytes; 128 entries PER SITE so loud sites cannot drown quiet ones).
    // Answers which site renders map country names and in what byte form.
    if (g_diag && site < kSiteCount) {
        static std::atomic<uint64_t> fstr_seen[16][128];
        static std::atomic<uint32_t> fstr_n[16];
        const uint8_t* q = reinterpret_cast<const uint8_t*>(p);
        if (q[0] >= 0x10 && q[0] <= 0x13) {
            uint64_t h = 1469598103934665603ull;
            for (int i = 0; i < 10; i++) {
                h ^= q[i];
                h *= 1099511628211ull;
                if (!q[i]) break;
            }
            uint32_t n = fstr_n[site].load(std::memory_order_relaxed);
            bool dup = false;
            for (uint32_t i = 0; i < n; i++)
                if (fstr_seen[site][i].load(std::memory_order_relaxed) == h) { dup = true; break; }
            if (!dup && n < 128) {
                fstr_seen[site][n].store(h, std::memory_order_relaxed);
                fstr_n[site].compare_exchange_strong(n, n + 1, std::memory_order_relaxed);
                char buf[112];
                size_t k = static_cast<size_t>(snprintf(buf, sizeof(buf), "site=%u ", site));
                for (int i = 0; i < 30 && k + 4 < sizeof(buf); i++) {
                    k += static_cast<size_t>(snprintf(buf + k, sizeof(buf) - k, "%02x ", q[i]));
                    if (!q[i]) break;
                }
                log_line("[eu4cjk] fetchstr %s\n", buf);
            }
        }
    }

    // EU4CJK_DIAG: for each distinct font object (up to 8) reaching a fetch
    // site, dump size/texid and the texture handler lookup result. The map
    // font (size 88) additionally dumps its object tail and a glyph node.
    if (g_diag && site < kSiteCount) {
        static const void* seen[8] = {};
        int slot = -1;
        for (int i = 0; i < 8; ++i) {
            if (seen[i] == font) { slot = i; break; }
            if (seen[i] == nullptr && slot < 0) slot = i;
        }
        if (slot >= 0 && seen[slot] != font) {
            seen[slot] = font;
            auto* f = static_cast<const uint8_t*>(font);
            int fsize = *reinterpret_cast<const int*>(f + 0x938);
            int texid = *reinterpret_cast<const int*>(f + 0x948);
            const void* tex = nullptr;
            uintptr_t gfx = *reinterpret_cast<const uintptr_t*>(f + 0x40);
            if (gfx) {
                uintptr_t texh =
                    *reinterpret_cast<const uintptr_t*>(gfx + 0x458);
                if (texh) {
                    using GetTexFn = void* (*)(void*, int);
                    auto get_tex = reinterpret_cast<GetTexFn>(0x22a8a2e);
                    tex = get_tex(reinterpret_cast<void*>(texh), texid);
                }
            }
            log_line("[eu4cjk] DIAG font slot%d site=%u font=%p size=%d"
                     " texid=%d tex=%p\n", slot, site, font, fsize, texid, tex);
            if (fsize == 88) {
                const void* g = eu4cjk::font::fetch_glyph_live(font, p, consumed);
                if (*consumed == 3 && g) {
                    auto* nd = static_cast<const uint8_t*>(g);
                    log_line("[eu4cjk]   mapfont node %p: %02x %02x %02x %02x"
                             " %02x %02x %02x %02x %02x %02x %02x %02x %02x"
                             " %02x %02x %02x\n", g,
                             nd[0], nd[1], nd[2], nd[3], nd[4], nd[5], nd[6],
                             nd[7], nd[8], nd[9], nd[10], nd[11], nd[12],
                             nd[13], nd[14], nd[15]);
                }
            }
        }
    }

    if (g_mode == mProbe) {
        // unreachable: probe mode uses the probe stubs; kept for safety
        *consumed = 1;
        return *reinterpret_cast<void* const*>(
            static_cast<const uint8_t*>(font) + 0x100
            + static_cast<size_t>(static_cast<uint8_t>(p[0])) * 8);
    }

    // 3D label builders: vanilla behaviour (no decode, no emission) so the
    // animated map-text path stays consistent; their labels render via RTS.
    if ((site == 0 || site == 1) && g_mode == mFull && !g_3d_decode
        && fvb_caller_is_3d_label()) {
        *consumed = 1;
        wrapfix::note_char(site, false);
        return *reinterpret_cast<void* const*>(
            static_cast<const uint8_t*>(font) + 0x100
            + static_cast<size_t>(static_cast<uint8_t>(p[0])) * 8);
    }

    const void* g = eu4cjk::font::fetch_glyph_live(font, p, consumed);
    if (*consumed == 3) {
        uint64_t n = g_escapes.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1)
            log_line("[eu4cjk] fetch: first escape decoded -> id=0x%04X,"
                     " glyph=%s\n",
                     // reconstruct id for the log via the same decode
                     static_cast<unsigned>(
                         eu4cjk::font::decode_id(reinterpret_cast<const uint8_t*>(p))),
                     g ? "FOUND" : "MISSING");
    }
    if (g_mode == mFull) {
        // wrap-fix gate flag: external CJK glyph (escape id > 0xFF) -> the
        // inline wrap gates treat this char as a break candidate.
        wrapfix::note_char(site, *consumed == 3 &&
            eu4cjk::font::decode_id(reinterpret_cast<const uint8_t*>(p)) > 0xFF);
    }

    if (g_mode == mHalf) {
        // decode + log only: behave exactly like the original instructions
        g = *reinterpret_cast<void* const*>(
            static_cast<const uint8_t*>(font) + 0x100
            + static_cast<size_t>(static_cast<uint8_t>(p[0])) * 8);
        *consumed = 1;
    } else if (g_mode == mFull && acc && *consumed == 3) {
        // keep the wrap string escape-intact for the emit loop
        append_byte(acc, static_cast<char>(p[1]));
        append_byte(acc, static_cast<char>(p[2]));
        g_appends.fetch_add(2, std::memory_order_relaxed);
    }

    if (site == 6 && *consumed == 3 && g_mode == mFull) {
        // RTS-1: engine copied the prefix at word_buf[copy_idx-1]; put the
        // payload right after it so strcat carries the full escape into the
        // line buffer (pText) that the second loop re-scans.
        auto* word_buf = reinterpret_cast<char*>(0x3345190);
        word_buf[copy_idx] = p[1];
        word_buf[copy_idx + 1] = p[2];
    }
    return g;
}
// CBitmapFont::RenderToScreen (0x20519f8): two glyph-fetch loops.
//   RTS-1 wrap loop (0x2052184): font=[rsp+0x30]->rdx, c=rcx, node->rbp,
//     s = r14 + sext(r13d) (tail: inc %r13d), word-buffer copy cursor r12
//     (buf[ebx]=c at head, r12=ebx+1, tail ebx=r12).
//   RTS-2 render loop (0x205307f): font=[rsp+0x30]->rdi, c=eax, node->r13,
//     s = pText(0x333d450) + sext(r15d) (tail 0x2053959: inc %r15d); engine
//     already uses add $2/$3 mid-loop for its own multi-byte tags.
constexpr uintptr_t kSiteRTS1 = 0x2052184;
constexpr size_t kLenRTS1 = 13;
constexpr uint8_t kOrigRTS1[kLenRTS1] = {
    0x48, 0x8B, 0x54, 0x24, 0x30,             // mov 0x30(%rsp),%rdx
    0x48, 0x8B, 0xAC, 0xCA, 0x00, 0x01, 0x00, 0x00, // mov 0x100(%rdx,%rcx,8),%rbp
};
constexpr uintptr_t kResumeRTS1 = 0x2052191;
constexpr uintptr_t kSiteRTS2 = 0x205307f;
constexpr size_t kLenRTS2 = 13;
constexpr uint8_t kOrigRTS2[kLenRTS2] = {
    0x48, 0x8B, 0x7C, 0x24, 0x30,             // mov 0x30(%rsp),%rdi
    0x4C, 0x8B, 0xAC, 0xC7, 0x00, 0x01, 0x00, 0x00, // mov 0x100(%rdi,%rax,8),%r13
};
constexpr uintptr_t kResumeRTS2 = 0x205308c;

bool install_probe(const char* name, uint32_t tag, uintptr_t site, size_t len,
                   const uint8_t* orig, uintptr_t resume)
{
    stubgen::StubSpec spec;
    spec.saves[0].reg = stubgen::Reg::Rax;
    spec.n_saves = 1;
    spec.args[0].kind = stubgen::StubSpec::Arg::Imm32;
    spec.args[0].imm = tag;
    spec.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
    spec.args[1].src = stubgen::Reg::Rax;
    spec.fn = reinterpret_cast<uintptr_t>(&eu4cjk_probe_hit);
    spec.replay = orig;
    spec.replay_len = len;
    spec.resume = resume;

    uintptr_t stub = stubgen::emit(spec);
    if (!stub || !stubgen::install_jmp(site, orig, len, stub)) {
        log_line("[eu4cjk] render %s: probe install failed\n", name);
        return false;
    }
    return true;
}
bool install_fetch(const char* name, uintptr_t site, size_t len,
                   const uint8_t* orig, uintptr_t resume, bool site_a)
{
    stubgen::StubSpec spec;
    spec.fn = reinterpret_cast<uintptr_t>(&eu4cjk_render_fetch);

    if (site_a) {
        spec.saves[0].reg = stubgen::Reg::Rax;   // &s[i], kept for the tail
        spec.saves[0].restore = false;           // final rax = byte (tail sets it)
        spec.n_saves = 1;
        spec.args[0].kind = stubgen::StubSpec::Arg::RbpSlot;
        spec.args[0].rbp_off = 0x70;             // [entry_rsp + 0x68] = font
        spec.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
        spec.args[1].src = stubgen::Reg::Rax;
        spec.args[2].kind = stubgen::StubSpec::Arg::FrameSlotLea;
        spec.args[2].frame_off = kConsumedSlot;
        spec.args[3].kind = stubgen::StubSpec::Arg::RbpLea;
        spec.args[3].rbp_off = 0x40;             // &wrap CString = entry_rsp+0x38
        spec.args[4].kind = stubgen::StubSpec::Arg::Imm32;
        spec.args[4].imm = 0;                    // site id
        spec.has_result = true;
        spec.result_reg = stubgen::Reg::R15;
        // tail: rcx = font; rax = byte  (register effects of the replaced code)
        static const uint8_t tail[] = {
            0x48, 0x8B, 0x4D, 0x70,             // mov rcx, [rbp+0x70]
            0x48, 0x8B, 0x44, 0x24, 0x00,       // mov rax, [rsp]      (&s[i])
            0x0F, 0xB6, 0x00,                   // movzbl (%rax), %eax
        };
        spec.tail = tail;
        spec.tail_len = sizeof(tail);
        spec.has_advance = true;
        spec.advance_frame_off = kConsumedSlot;
        spec.advance_reg = stubgen::Reg::R14;
    } else {
        spec.args[0].kind = stubgen::StubSpec::Arg::RegSrc;
        spec.args[0].src = stubgen::Reg::R12;    // font
        spec.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
        spec.args[1].src = stubgen::Reg::Rax;    // &s[i]
        spec.args[2].kind = stubgen::StubSpec::Arg::FrameSlotLea;
        spec.args[2].frame_off = kConsumedSlot;
        spec.args[3].kind = stubgen::StubSpec::Arg::Imm32;  // acc = nullptr:
        spec.args[3].imm = 0;                               // emit loop rebuilds nothing
        spec.args[4].kind = stubgen::StubSpec::Arg::Imm32;
        spec.args[4].imm = 1;                    // site id
        // result stays in rax (original destination of the load)
        spec.has_advance = true;
        spec.advance_frame_off = kConsumedSlot;
        spec.advance_reg = stubgen::Reg::Rbx;
    }
    spec.resume = resume;

    uintptr_t stub = stubgen::emit(spec);
    if (!stub || !stubgen::install_jmp(site, orig, len, stub)) {
        log_line("[eu4cjk] render %s: fetch install failed\n", name);
        return false;
    }
    log_line("[eu4cjk] render %s: fetch stub @ 0x%lx (site 0x%lx)\n",
             name, static_cast<unsigned long>(stub),
             static_cast<unsigned long>(site));
    return true;
}

bool install_fetch_r2t(const char* name, uintptr_t site, size_t len,
                       const uint8_t* orig, uintptr_t resume, bool wrap_loop)
{
    stubgen::StubSpec spec;
    spec.fn = reinterpret_cast<uintptr_t>(&eu4cjk_render_fetch);

    if (wrap_loop) {
        // R2T-1: font [entry_rsp+0x38] -> rbp_off 0x40; wrap accumulator
        // &CString at entry_rsp+0x58 -> rbp_off 0x60; node lands in rbp.
        spec.saves[0].reg = stubgen::Reg::Rax;   // &s[i], kept for the tail
        spec.saves[0].restore = false;
        spec.n_saves = 1;
        spec.args[0].kind = stubgen::StubSpec::Arg::RbpSlot;
        spec.args[0].rbp_off = 0x40;             // [entry_rsp + 0x38] = font
        spec.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
        spec.args[1].src = stubgen::Reg::Rax;
        spec.args[2].kind = stubgen::StubSpec::Arg::FrameSlotLea;
        spec.args[2].frame_off = kConsumedSlot;
        spec.args[3].kind = stubgen::StubSpec::Arg::RbpLea;
        spec.args[3].rbp_off = 0x60;             // &wrap CString = entry_rsp+0x58
        spec.args[4].kind = stubgen::StubSpec::Arg::Imm32;
        spec.args[4].imm = 2;                    // site id
        spec.has_result = true;
        spec.result_reg = stubgen::Reg::R11;
        // tail: rcx = font; rax = byte  (register effects of the replaced code)
        static const uint8_t tail[] = {
            0x48, 0x8B, 0x4D, 0x40,             // mov rcx, [rbp+0x40]
            0x48, 0x8B, 0x04, 0x24,             // mov rax, [rsp]      (&s[i])
            0x0F, 0xB6, 0x00,                   // movzbl (%rax), %eax
        };
        spec.tail = tail;
        spec.tail_len = sizeof(tail);
        // engine-context: rbp = node (frame anchor forbids the in-frame tail)
        static const uint8_t post[] = {
            0x4C, 0x89, 0xDD,                   // mov rbp, r11
        };
        spec.post_tail = post;
        spec.post_tail_len = sizeof(post);
        spec.has_advance = true;
        spec.advance_frame_off = kConsumedSlot;
        spec.advance_reg = stubgen::Reg::R14;
    } else {
        // R2T-2: font in r13, destination r11 (result_reg), emit loop
        // re-decodes the rebuilt string's escapes itself - no accumulator.
        spec.args[0].kind = stubgen::StubSpec::Arg::RegSrc;
        spec.args[0].src = stubgen::Reg::R13;   // font
        spec.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
        spec.args[1].src = stubgen::Reg::Rax;   // &s[i]
        spec.args[2].kind = stubgen::StubSpec::Arg::FrameSlotLea;
        spec.args[2].frame_off = kConsumedSlot;
        spec.args[3].kind = stubgen::StubSpec::Arg::Imm32;  // acc = nullptr
        spec.args[3].imm = 0;
        spec.args[4].kind = stubgen::StubSpec::Arg::Imm32;
        spec.args[4].imm = 3;                    // site id
        spec.has_result = true;
        spec.result_reg = stubgen::Reg::R11;
        spec.has_advance = true;
        spec.advance_frame_off = kConsumedSlot;
        spec.advance_reg = stubgen::Reg::R14;
    }
    spec.resume = resume;

    uintptr_t stub = stubgen::emit(spec);
    if (!stub || !stubgen::install_jmp(site, orig, len, stub)) {
        log_line("[eu4cjk] render %s: fetch install failed\n", name);
        return false;
    }
    log_line("[eu4cjk] render %s: fetch stub @ 0x%lx (site 0x%lx)\n",
             name, static_cast<unsigned long>(stub),
             static_cast<unsigned long>(site));
    return true;
}

bool install_fetch_r3d(const char* name, uintptr_t site, size_t len,
                       const uint8_t* orig, uintptr_t resume, bool wrap_loop)
{
    stubgen::StubSpec spec;
    spec.fn = reinterpret_cast<uintptr_t>(&eu4cjk_render_fetch);

    // Both R3D sites: font in r12, index in r15d.
    spec.args[0].kind = stubgen::StubSpec::Arg::RegSrc;
    spec.args[0].src = stubgen::Reg::R12;   // font
    spec.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
    spec.args[1].src = stubgen::Reg::Rax;   // &s[i]
    spec.args[2].kind = stubgen::StubSpec::Arg::FrameSlotLea;
    spec.args[2].frame_off = kConsumedSlot;
    if (wrap_loop) {
        // R3D-1: wrap accumulator &CString at entry_rsp+0x58 -> rbp_off 0x60
        spec.args[3].kind = stubgen::StubSpec::Arg::RbpLea;
        spec.args[3].rbp_off = 0x60;
        spec.args[4].kind = stubgen::StubSpec::Arg::Imm32;
        spec.args[4].imm = 4;                // site id
        spec.has_result = true;
        spec.result_reg = stubgen::Reg::Rbx;
    } else {
        // R3D-2: emit loop re-decodes the rebuilt string itself
        spec.args[3].kind = stubgen::StubSpec::Arg::Imm32;  // acc = nullptr
        spec.args[3].imm = 0;
        spec.args[4].kind = stubgen::StubSpec::Arg::Imm32;
        spec.args[4].imm = 5;                // site id
        spec.has_result = true;
        spec.result_reg = stubgen::Reg::Rax;
    }
    spec.has_advance = true;
    spec.advance_frame_off = kConsumedSlot;
    spec.advance_reg = stubgen::Reg::R15;
    spec.resume = resume;

    uintptr_t stub = stubgen::emit(spec);
    if (!stub || !stubgen::install_jmp(site, orig, len, stub)) {
        log_line("[eu4cjk] render %s: fetch install failed\n", name);
        return false;
    }
    log_line("[eu4cjk] render %s: fetch stub @ 0x%lx (site 0x%lx)\n",
             name, static_cast<unsigned long>(stub),
             static_cast<unsigned long>(site));
    return true;
}

bool install_fetch_rts1(const char* name)
{
    stubgen::StubSpec spec;
    spec.fn = reinterpret_cast<uintptr_t>(&eu4cjk_render_fetch);

    spec.args[0].kind = stubgen::StubSpec::Arg::RbpSlot;
    spec.args[0].rbp_off = 0x38;              // [entry_rsp+0x30] = font
    spec.args[1].kind = stubgen::StubSpec::Arg::IdxSextSum;
    spec.args[1].idx = stubgen::Reg::R13;     // p = r14 + sext(r13d) = &s[i]
    spec.args[1].base = stubgen::Reg::R14;
    spec.args[1].has_base = true;
    spec.args[2].kind = stubgen::StubSpec::Arg::FrameSlotLea;
    spec.args[2].frame_off = kConsumedSlot;
    spec.args[3].kind = stubgen::StubSpec::Arg::Imm32;   // acc = nullptr:
    spec.args[3].imm = 0;                                // payload handled via
    spec.args[4].kind = stubgen::StubSpec::Arg::Imm32;   // the word buffer below
    spec.args[4].imm = 6;                               // site id
    spec.args[5].kind = stubgen::StubSpec::Arg::RegSrc;
    spec.args[5].src = stubgen::Reg::R12;     // copy cursor for word_buf write
    spec.has_result = true;
    spec.result_reg = stubgen::Reg::R11;
    // tail: rdx = font (register effect of the replaced mov)
    static const uint8_t tail[] = {
        0x48, 0x8B, 0x55, 0x38,               // mov rdx, [rbp+0x38]
    };
    spec.tail = tail;
    spec.tail_len = sizeof(tail);
    // engine-context: rbp = node (frame anchor forbids the in-frame tail)
    static const uint8_t post[] = {
        0x4C, 0x89, 0xDD,                     // mov rbp, r11
    };
    spec.post_tail = post;
    spec.post_tail_len = sizeof(post);
    spec.has_advance = true;                  // string cursor r13d += 2 ...
    spec.advance_frame_off = kConsumedSlot;
    spec.advance_reg = stubgen::Reg::R13;
    spec.has_advance2 = true;                 // ... and copy cursor r12d += 2
    spec.advance2_reg = stubgen::Reg::R12;    // (tail ebx=r12 keeps them in step)
    spec.resume = kResumeRTS1;

    uintptr_t stub = stubgen::emit(spec);
    if (!stub || !stubgen::install_jmp(kSiteRTS1, kOrigRTS1, kLenRTS1, stub)) {
        log_line("[eu4cjk] render %s: fetch install failed\n", name);
        return false;
    }
    log_line("[eu4cjk] render %s: fetch stub @ 0x%lx (site 0x%lx)\n",
             name, static_cast<unsigned long>(stub),
             static_cast<unsigned long>(kSiteRTS1));
    return true;
}

bool install_fetch_rts2(const char* name)
{
    stubgen::StubSpec spec;
    spec.fn = reinterpret_cast<uintptr_t>(&eu4cjk_render_fetch);

    spec.args[0].kind = stubgen::StubSpec::Arg::RbpSlot;
    spec.args[0].rbp_off = 0x38;              // [entry_rsp+0x30] = font
    spec.args[1].kind = stubgen::StubSpec::Arg::IdxSextSum;
    spec.args[1].idx = stubgen::Reg::R15;     // p = pText + sext(r15d)
    spec.args[1].base_imm = 0x333d450;
    spec.args[1].has_base = false;
    spec.args[2].kind = stubgen::StubSpec::Arg::FrameSlotLea;
    spec.args[2].frame_off = kConsumedSlot;
    spec.args[3].kind = stubgen::StubSpec::Arg::Imm32;   // acc = nullptr
    spec.args[3].imm = 0;
    spec.args[4].kind = stubgen::StubSpec::Arg::Imm32;
    spec.args[4].imm = 7;                     // site id
    spec.has_result = true;
    spec.result_reg = stubgen::Reg::R11;
    // tail: restore the raw char into eax BEFORE the index advance: the C++
    // call clobbers rax, but the engine's pass-2 newline/directive checks
    // (0x20533e1 cmp $0xa,%al) still read it - a stale al silently drops
    // every '\n' the layout pass inserted (CJK rendered as one long line).
    // r15d writes zero-extend, so 0x333d450(%r15) = pText[orig_idx].
    // Then rdi = font (register effect of the replaced mov).
    static const uint8_t tail[] = {
        0x41, 0x0F, 0xB6, 0x87, 0x50, 0xD4, 0x33, 0x03, // movzbl 0x333d450(%r15),%eax
        0x48, 0x8B, 0x7D, 0x38,                          // mov rdi, [rbp+0x38]
    };
    spec.tail = tail;
    spec.tail_len = sizeof(tail);
    // engine-context: r13 = node
    static const uint8_t post[] = {
        0x4D, 0x89, 0xDD,                     // mov r13, r11
    };
    spec.post_tail = post;
    spec.post_tail_len = sizeof(post);
    spec.has_advance = true;
    spec.advance_frame_off = kConsumedSlot;
    spec.advance_reg = stubgen::Reg::R15;
    spec.resume = kResumeRTS2;

    uintptr_t stub = stubgen::emit(spec);
    if (!stub || !stubgen::install_jmp(kSiteRTS2, kOrigRTS2, kLenRTS2, stub)) {
        log_line("[eu4cjk] render %s: fetch install failed\n", name);
        return false;
    }
    log_line("[eu4cjk] render %s: fetch stub @ 0x%lx (site 0x%lx)\n",
             name, static_cast<unsigned long>(stub),
             static_cast<unsigned long>(kSiteRTS2));
    return true;
}

[[maybe_unused]] bool install_fetch_count(const char* name, uintptr_t site, size_t len,
                         const uint8_t* orig, uintptr_t resume, uint32_t tag)
{
    // Sizing loops: font r12, &s[i] in rax, node into rax, index ebx.
    stubgen::StubSpec spec;
    spec.fn = reinterpret_cast<uintptr_t>(&eu4cjk_render_fetch);
    spec.args[0].kind = stubgen::StubSpec::Arg::RegSrc;
    spec.args[0].src = stubgen::Reg::R12;   // font
    spec.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
    spec.args[1].src = stubgen::Reg::Rax;   // &s[i]
    spec.args[2].kind = stubgen::StubSpec::Arg::FrameSlotLea;
    spec.args[2].frame_off = kConsumedSlot;
    spec.args[3].kind = stubgen::StubSpec::Arg::Imm32;  // acc = nullptr
    spec.args[3].imm = 0;
    spec.args[4].kind = stubgen::StubSpec::Arg::Imm32;
    spec.args[4].imm = tag;
    spec.has_result = true;
    spec.result_reg = stubgen::Reg::Rax;
    spec.has_advance = true;
    spec.advance_frame_off = kConsumedSlot;
    spec.advance_reg = stubgen::Reg::Rbx;
    spec.resume = resume;

    uintptr_t stub = stubgen::emit(spec);
    if (!stub || !stubgen::install_jmp(site, orig, len, stub)) {
        log_line("[eu4cjk] render %s: fetch install failed\n", name);
        return false;
    }
    log_line("[eu4cjk] render %s: fetch stub @ 0x%lx (site 0x%lx)\n",
             name, static_cast<unsigned long>(stub),
             static_cast<unsigned long>(site));
    return true;
}
// FillVertexBuffer entry recorder: replays the first 5 bytes (push rbp;
// push r15; push r14) of the prologue, hands the caller return address
// ([entry_rsp+0]) to eu4cjk_fillvb_entry, then resumes at 0x205544d.
//   2055448: 55            push %rbp
//   2055449: 41 57         push %r15
//   205544b: 41 56         push %r14
constexpr uintptr_t kSiteFVEnt = 0x2055448;
constexpr size_t kLenFVEnt = 5;
constexpr uint8_t kOrigFVEnt[kLenFVEnt] = { 0x55, 0x41, 0x57, 0x41, 0x56 };
constexpr uintptr_t kResumeFVEnt = 0x205544d;

bool install_fvb_entry()
{
    stubgen::StubSpec spec;
    spec.args[0].kind = stubgen::StubSpec::Arg::RbpSlot;
    spec.args[0].rbp_off = 8;          // [rbp+8] = [entry_rsp+0] = caller ret
    spec.fn = reinterpret_cast<uintptr_t>(&eu4cjk_fillvb_entry);
    spec.save_volatile = true;         // FillVB entry args are live
    spec.replay = kOrigFVEnt;
    spec.replay_len = kLenFVEnt;
    spec.resume = kResumeFVEnt;
    uintptr_t stub = stubgen::emit(spec);
    if (!stub || !stubgen::install_jmp(kSiteFVEnt, kOrigFVEnt, kLenFVEnt, stub)) {
        log_line("[eu4cjk] render FVB-entry: install failed\n");
        return false;
    }
    log_line("[eu4cjk] render FVB-entry: stub @ 0x%lx\n",
             static_cast<unsigned long>(stub));
    return true;
}
} // namespace
