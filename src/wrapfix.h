#pragma once

#include <cstddef>
#include <cstdint>

namespace eu4cjk::wrapfix {

// M4-P1d CJK line-wrap fix (upstream mainTextProc3 equivalent).
//
// Root cause: CBitmapFont::ParseFontFile forces glyph node field +6 (bitmap
// height) to 0 for id 0x20 (space) and the text functions reuse that field
// as the "break candidate" marker: every wrap gate is
//   cmpw $0,0x6(node)  + branch
// so only spaces ever reach the inline overflow/wrap blocks. External CJK
// glyph nodes carry their true height -> gates always skip -> CJK text never
// wraps (tooltips, event dialogs, ...). Upstream Windows fix (mainTextProc3)
// forces code points > 0xFF into the space path; the Linux port reproduces
// the same semantics with a per-gate CJK flag fed by the hooked fetch sites.
//
// Gate bits (EU4CJK_WRAP_MASK bitmask, default 0x07):
//   0 FillVertexBuffer  1 RenderToTexture  2 GetWidthOfString
//   3 RenderToScreen    4 Render3d
//   5-7 reserved (AddNameArea / CurveText / AddNudgedNames map paths:
//   their gates use setg/jle forms entangled with curve emission - map names
//   already render correctly, so they stay unpatched until proven needed).
// bit3 (RTS) on by default: its native non-space hard-wrap is dead for CJK
// (guarded by pText non-empty at 0x2052244 - pure CJK never flushes a word,
// so pText stays empty). Event dialogs / scenario intro lay out via RTS.
// Short strings (map labels) take the fits-flush path: pText content is
// identical, zero visual change; only over-wide lines gain '\n' breaks.
constexpr uint32_t kDefaultMask = 0x0F;
constexpr int kGateBits = 5;

// Called by the render fetch helper after every decoded character: records
// whether the CURRENT char is an external CJK glyph (escape id > 0xFF) for
// the gate fed by this fetch site. site = render.cpp fetch-site id.
void note_char(uint32_t site, bool cjk);

// Diagnostics: how many CJK chars each fetch site decoded (wrap gate feed).
uint64_t note_cjk_count(uint32_t site);

// Fixed assembled length of each gate stub (build_stub output).
size_t stub_len(int bit);

// Assembles the gate stub for `bit` assuming it will execute at stub_addr
// with its flag byte at flag_addr (both must be within +-2GB). Writes to out
// (capacity >= stub_len(bit)). Returns the length, 0 on bad input/overflow.
size_t build_stub(int bit, uintptr_t flag_addr, uintptr_t stub_addr,
                  uint8_t* out);

// Parameterized builder (selftest): same encoding as build_stub but with
// explicit taken/untaken jump targets (build_stub = the engine-target
// specialization). GW's commit path is internal and ignores `taken`.
size_t build_stub_ex(int bit, uintptr_t flag_addr, uintptr_t stub_addr,
                     uintptr_t taken, uintptr_t untaken, uint8_t* out);

// Patches the enabled gates in the live game image. Idempotent. Honors
// EU4CJK_WRAP0 (disable all) and EU4CJK_WRAP_MASK (bitmask override).
bool install();

// Selftest hooks: bind/read the flag storage without touching the game.
void bind_flags_for_test(uint8_t* flags);

} // namespace eu4cjk::wrapfix
