#pragma once

#include <cstddef>
#include <cstdint>

namespace eu4cjk::render {

// M3-T3: divert the two FillVertexBuffer char-fetch sites.
// Modes (env EU4CJK_RENDER, default "full"):
//   probe - count hits, replay original instructions (zero behavior change)
//   half  - decode escapes and log, but return the original inline glyph and
//           never advance the cursor (zero visual change)
//   full  - decode, return the external glyph, advance the cursor by 2
bool install();

// Builds the CurveText t-numerator stub (compensates the CT-1 +2 cursor
// advance in the curve interpolation parameter) into out[cap]; returns the
// byte length (0 on overflow). Shared by the installer and the offline
// selftest.
size_t build_ct_tdiv_stub(uint8_t* out, size_t cap, uintptr_t resume);

// Builds the CurveText single-glyph sampler fix stub (mirrors the
// first-glyph curve samples into the last-glyph slots when n==6).
size_t build_ct_single_stub(uint8_t* out, size_t cap, uintptr_t resume);

}
