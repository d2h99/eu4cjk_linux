#pragma once

// Shared harness for the offline selftest family (split of the former
// selftest.cpp monolith, 2026-09-28): check bookkeeping, cross-section
// fake-font/glyph state and helper landing pads. C++17 inline variables
// keep ONE shared instance across the test TUs (same semantics as the old
// single-TU anonymous namespace).

#include "../src/byte_pattern.h"
#include "../src/injector.hpp"
#include "../src/font.h"
#include "../src/stubgen.h"
#include "../src/wrapfix.h"
#include "../src/render.h"

#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

namespace eu4cjk_test {

extern int g_fails;
inline volatile int g_save_stub_landed = 0;

void run_fetch_tests();
void run_gate_tests();
void run_stub_tests();
void run_full_stub_tests();
void run_measure_stub_tests();
void run_wrap_stub_tests();
void run_ct_tdiv_tests();
void run_ct_single_tests();
void run_escape_tests();

const char kMarker[] = "EU4CJK!SELFTERM#MARKER$0123456789";

inline void check(bool ok, const char* name)
{
    if (!ok) ++g_fails;
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
}

inline bool within_marker(uintptr_t p)
{
    return p >= reinterpret_cast<uintptr_t>(kMarker)
        && p < reinterpret_cast<uintptr_t>(kMarker) + sizeof(kMarker);
}

alignas(8) inline uint8_t g_font[0x900];
inline const void* g_ext[65536];

inline uint32_t g_consumed;
inline const void* slot_at(uint8_t id)
{
    return *reinterpret_cast<void* const*>(g_font + 0x100 + static_cast<size_t>(id) * 8);
}

inline void fetch_case(const char* name, const char* p, uint32_t want_consumed,
                const void* want)
{
    g_consumed = 0xDeadBeef;
    const void* got = eu4cjk::font::fetch_glyph(g_font, g_ext, p, &g_consumed);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "fetch_glyph: %s", name);
    check(got == want && g_consumed == want_consumed, buf);
}

// ---- per-font external table isolation (M3-T3 fix) -------------------------

// ---- M3-T1: stubgen offline execution ------------------------------------

inline uint64_t g_stub_hits;
inline uint32_t g_stub_tag;
inline uint8_t g_stub_byte;
inline const void* g_test_ext[65536];
inline void* g_got_acc;
inline const void* g_got_sso;
inline volatile uintptr_t g_stub_a, g_stub_b, g_stub_c; // immune to asm-input staging overlap
inline volatile uint64_t g_out15, g_outcx, g_outax, g_out_sp, g_outb, g_outbx;
inline volatile uint64_t g_outbp, g_outcx2, g_outax2, g_out_sp2;

inline __attribute__((noinline)) void test_helper(uint32_t tag, const char* p)
{
    ++g_stub_hits;
    g_stub_tag = tag;
    g_stub_byte = static_cast<uint8_t>(*p);
}

// Mimics render.cpp's eu4cjk_render_fetch against a controllable table.
// Records the marshalled acc/sso so the RbpLea/RegSrc wiring is verified too.
inline __attribute__((noinline)) const void* test_fetch(const void* f, const char* p,
                                                 uint32_t* consumed, void* acc)
{
    ++g_stub_hits;
    g_got_acc = acc;
    return eu4cjk::font::fetch_glyph(f, g_test_ext, p, consumed);
}

inline __attribute__((noinline)) void ret_gadget() {}

// Real site bytes (see render.cpp).
const uint8_t kOrigA[16] = {
    0x0F, 0xB6, 0x00,
    0x48, 0x8B, 0x4C, 0x24, 0x68,
    0x4C, 0x8B, 0xBC, 0xC1, 0x00, 0x01, 0x00, 0x00,
};
const uint8_t kOrigB[11] = {
    0x0F, 0xB6, 0x00,
    0x49, 0x8B, 0x84, 0xC4, 0x00, 0x01, 0x00, 0x00,
};

// ---- M3 phase 2: full-logic fetch stubs (site A and B shapes) -------------
//
// NOTE: all stubs are EMITTED before any asm driver executes. GCC stages
// extended-asm inputs through stack slots that proved able to overlap spec
// objects living across an asm block (found the hard way: fields of a spec
// built after an asm test were clobbered). Emit-then-run keeps spec
// lifetimes disjoint from stub execution.

// ---- M3-T4: measurement-family stub shapes ---------------------------------
//
// GW: font rdi, p = rbx+sext(r15d), node -> rbp (post_tail), rsi restored to
//     the byte via a [rbx+r15] tail, index r14d advance (register).
// GH: font rbx, p = rax, node -> rax (helper return), index ebp advance via
//     advance_mem on the stub's pushed-rbp slot ([rbp+0]).
// GR: font r13, p = rax, node -> rbp (post_tail), index dword [entry_rsp+4]
//     advance via advance_mem into the engine frame slot.
inline volatile uintptr_t g_stub_gw, g_stub_gh, g_stub_gr;
inline volatile uint64_t g_mgw_rbp, g_mgw_rsi, g_mgw_rdi, g_mgh_rax, g_mgh_rbp,
    g_mgr_rbp, g_mgr_slot_after;

// ---- M4-P1d: wrap-fix gate stubs -------------------------------------------
//
// The gates patch the engine's inline `cmpw $0,0x6(node)` wrap checks: a
// per-site flag byte (set by the fetch helpers via note_char) marks the
// current char as external CJK, and the stub routes CJK chars into the
// space/break-candidate path (upstream mainTextProc3 semantics).

inline volatile int g_wrap_land = 0;   // 1 = taken (break path), 2 = untaken

inline __attribute__((noinline)) void wrap_pad_taken() { g_wrap_land = 1; }
inline __attribute__((noinline)) void wrap_pad_untaken() { g_wrap_land = 2; }

} // namespace eu4cjk_test

using namespace eu4cjk_test;
