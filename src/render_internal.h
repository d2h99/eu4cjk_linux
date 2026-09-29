#pragma once

// Internal shared declarations for the eu4cjk::render family (render.cpp
// orchestrates; render_fetch/trace/measure/mapfix/curve.cpp implement).
// Pure code-motion split of the former render.cpp monolith (2026-09-28);
// no behavior change. Build uses -fvisibility=hidden, so the non-static
// linkage here stays hidden from the .so ABI.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "stubgen.h"

namespace eu4cjk::render {

// site ids 10-14: GW, GH, GA, GR, GAR (measurement family); 15: CT-1 CurveText
constexpr uint32_t kSiteCount = 16;
constexpr size_t kConsumedSlot = 0x18; // frame slot of the u32 'consumed'

enum Mode { mProbe, mHalf, mFull, mR2TOnly };
extern Mode g_mode;
extern bool g_3d_decode;
extern bool g_diag;
extern std::atomic<uint64_t> g_hits[2];
extern std::atomic<uint64_t> g_escapes;
extern std::atomic<uint64_t> g_appends;
extern std::atomic<uint64_t> g_site_hits[16];

extern "C" const void* eu4cjk_render_fetch(const void* font, const char* p,
                                           uint32_t* consumed, void* acc,
                                           uint32_t site, uintptr_t copy_idx);
extern "C" void eu4cjk_case_probe(void* self);
extern "C" void eu4cjk_case_probe_lo(void* self);

bool install_trace(const char* name, uint32_t tag, uintptr_t site, size_t len,
                   const uint8_t* orig, uintptr_t resume);
bool install_probe(const char* name, uint32_t tag, uintptr_t site, size_t len,
                   const uint8_t* orig, uintptr_t resume);
bool install_fetch(const char* name, uintptr_t site, size_t len,
                   const uint8_t* orig, uintptr_t resume, bool site_a);
bool install_fetch_r2t(const char* name, uintptr_t site, size_t len,
                       const uint8_t* orig, uintptr_t resume, bool wrap_loop);
bool install_fetch_r3d(const char* name, uintptr_t site, size_t len,
                       const uint8_t* orig, uintptr_t resume, bool wrap_loop);
bool install_fetch_rts1(const char* name);
bool install_fetch_rts2(const char* name);
bool install_fetch_count(const char* name, uintptr_t site, size_t len,
                         const uint8_t* orig, uintptr_t resume, uint32_t tag);
bool install_fvb_entry();
bool install_measure_common(const char* name, uintptr_t site, size_t len,
                            const uint8_t* orig, uintptr_t resume,
                            const stubgen::StubSpec& spec_tmpl);
bool install_measure_gw(const char* name);
bool install_measure_gh(const char* name);
bool install_measure_ga(const char* name);
bool install_measure_gr(const char* name);
bool install_measure_gar(const char* name);
bool install_measure_ct(const char* name);
bool install_case_fix(const char* name, uintptr_t site, const uint8_t* orig,
                      uintptr_t resume, uintptr_t conv_target);
bool install_case_probe_at(const char* name, uintptr_t entry,
                           uintptr_t resume, void (*probe)(void*));
bool install_split_fix();
bool install_ct_tdiv_fix();
bool install_ct_single_fix();

void install_entry_traces();
void log_trace_final();
void log_caller_histogram();

} // namespace eu4cjk::render
