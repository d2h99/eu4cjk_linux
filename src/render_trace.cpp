// Diagnostic entry traces: counters + first-hit string dumps at renderer
// entries (R2T/RTS/R3D/Buckets/SetText*/GetWidth/GetHeight), installer and
// the exit summary. (Split of the former render.cpp monolith, 2026-09-28.)
#include "render_internal.h"
#include "log.h"
#include "stubgen.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace eu4cjk::render {

// ---- diagnostic entry traces (which renderer draws what) -------------------
//
// Probes at function entries: count calls; for string-bearing entry args
// sample the first bytes. Pure observation: replay + resume, no effects.
//   mode 0: entry counter only
//   mode 1: p = const CString*   -> data = *(char* const*)p
//   mode 2: p = const CTextBlock*-> data = *(char* const*)(p + 0x30)
//   mode 3: p = const char*
enum { kTraceN = 8 };
static std::atomic<uint64_t> g_trace[kTraceN];
static const char* const kTraceName[kTraceN] = {
    "R2T-entry", "RTS-entry", "R3D-entry", "Buckets-entry",
    "SetTextTB", "SetTextCS", "GetWidth", "GetHeight",
};
static const uint32_t kTraceMode[kTraceN] = { 0, 1, 1, 0, 2, 1, 3, 1 };

extern "C" void eu4cjk_trace_hit(uint32_t tag, const void* p)
{
    if (tag >= kTraceN) return;
    uint64_t n = g_trace[tag].fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 3) return;
    const char* s = nullptr;
    switch (kTraceMode[tag]) {
    case 1: s = p ? *static_cast<const char* const*>(p) : nullptr; break;
    case 2: s = p ? *reinterpret_cast<const char* const*>(
                        static_cast<const uint8_t*>(p) + 0x30) : nullptr; break;
    case 3: s = static_cast<const char*>(p); break;
    default: break;
    }
    if (!s) {
        log_line("[eu4cjk] trace[%s] hit#%llu (entry)\n", kTraceName[tag],
                 static_cast<unsigned long long>(n));
        return;
    }
    char buf[128];
    size_t o = 0;
    for (int i = 0; i < 24 && s[i] && o + 8 < sizeof(buf); ++i)
        o += static_cast<size_t>(snprintf(buf + o, sizeof(buf) - o, "%02X ",
                                          static_cast<unsigned>(
                                              static_cast<uint8_t>(s[i]))));
    log_line("[eu4cjk] trace[%s] hit#%llu: %s\n", kTraceName[tag],
             static_cast<unsigned long long>(n), buf);
}

bool install_trace(const char* name, uint32_t tag, uintptr_t site, size_t len,
                   const uint8_t* orig, uintptr_t resume)
{
    stubgen::StubSpec spec;
    spec.args[0].kind = stubgen::StubSpec::Arg::Imm32;
    spec.args[0].imm = tag;
    spec.args[1].kind = stubgen::StubSpec::Arg::RegSrc;
    spec.args[1].src = stubgen::Reg::Rsi;
    spec.fn = reinterpret_cast<uintptr_t>(&eu4cjk_trace_hit);
    spec.save_volatile = true;   // entry args (rdi/rsi/.../xmm0-3) are live
    spec.replay = orig;
    spec.replay_len = len;
    spec.resume = resume;
    uintptr_t stub = stubgen::emit(spec);
    if (!stub || !stubgen::install_jmp(site, orig, len, stub)) {
        log_line("[eu4cjk] trace %s: install failed\n", name);
        return false;
    }
    return true;
}

// Release default: OFF (pure forensics, ~0.1-0.5% CPU in UI-heavy scenes,
// 9 fewer engine patch points). EU4CJK_TRACE=1 opts back in (full mode only).
static bool g_traces_installed = false;

void install_entry_traces()
{
    const char* t = std::getenv("EU4CJK_TRACE");
    if (!t || std::strcmp(t, "1") != 0 || g_mode != mFull) return;
    g_traces_installed = true;
    static const uint8_t kPro7[7] = { 0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55 };
    static const uint8_t kSetTB[8] = { 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54 };
    static const uint8_t kSetCS[7] = { 0x41, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x28 };
    struct T { const char* n; uintptr_t a; size_t l; const uint8_t* o; };
    static const T ts[kTraceN] = {
        { "R2T-entry",   0x204e886, 7, kPro7 },
        { "RTS-entry",   0x20519f8, 7, kPro7 },
        { "R3D-entry",   0x205449a, 7, kPro7 },
        { "Buckets-entry", 0x202a35c, 7, kPro7 },
        { "SetTextTB",   0x202a24c, 8, kSetTB },
        { "SetTextCS",   0x202a5b4, 7, kSetCS },
        { "GetWidth",    0x2053f7c, 7, kPro7 },
        { "GetHeight",   0x2053b88, 7, kPro7 },
    };
    uint32_t n_ok = 0;
    for (uint32_t i = 0; i < kTraceN; ++i)
        n_ok += install_trace(ts[i].n, i, ts[i].a, ts[i].l, ts[i].o,
                              ts[i].a + ts[i].l);
    log_line("[eu4cjk] trace: %u/%u entry probes installed\n", n_ok,
             static_cast<unsigned>(kTraceN));
}

void log_trace_final()
{
    if (!g_traces_installed) return;  // release default: probes never ran
    log_line("[eu4cjk] trace final: R2T=%llu RTS=%llu R3D=%llu Buckets=%llu"
             " SetTextTB=%llu SetTextCS=%llu GetWidth=%llu GetHeight=%llu\n",
             static_cast<unsigned long long>(g_trace[0].load()),
             static_cast<unsigned long long>(g_trace[1].load()),
             static_cast<unsigned long long>(g_trace[2].load()),
             static_cast<unsigned long long>(g_trace[3].load()),
             static_cast<unsigned long long>(g_trace[4].load()),
             static_cast<unsigned long long>(g_trace[5].load()),
             static_cast<unsigned long long>(g_trace[6].load()),
             static_cast<unsigned long long>(g_trace[7].load()));
}

} // namespace eu4cjk::render
