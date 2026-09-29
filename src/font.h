#pragma once

#include <cstdint>

namespace eu4cjk::font {

// As laid out by the M2 relay stub on its stack frame ([rsp+0]=id,
// [rsp+4..0x11]=f[7], [rsp+0x18]=font). `font` is CBitmapFont* `this`
// captured from [entry_rsp+8] (see 0x204e380 in ParseFontFile).
struct GateArgs {
    uint32_t id;
    uint16_t f[7];
    const void* font;
};

extern "C" int eu4cjk_glyph_gate(const GateArgs* a);

bool install_glyph_gate();

// Global totals across all per-font external tables.
uint32_t stored_count();
uint32_t duplicate_count();
uint32_t font_registry_count();

// M3-T2: decode one "character" of an escaped byte stream and fetch its glyph
// node. Pure and unit-testable: font = CBitmapFont*, external = 65536-slot
// table (may be null -> external ids resolve to null). On return *consumed is
// 1 (plain byte, inline slot at font+0x100+b*8, may be null) or 3 (escape
// prefix 0x10..0x13 + low + high -> decoded id -> external[id]).
// Protocol constants mirror upstream escape_tool.cpp decode exactly.
const void* fetch_glyph(const void* font, const void* const* external,
                        const char* p, uint32_t* consumed);

// Same, but backed by the .so's own external table (for render stubs).
const void* fetch_glyph_live(const void* font, const char* p, uint32_t* consumed);

// Diagnostic: reconstruct the 16-bit id from an escape triple (p[0] is a
// prefix 0x10..0x13). Applies the same dead-zone fallback as fetch_glyph.
uint32_t decode_id(const uint8_t* p);

}
