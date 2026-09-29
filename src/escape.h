#pragma once

// M5: Linux port of the upstream escape_tool converters (Plugin64/
// escape_tool.cpp). The engine's internal text is the escaped byte
// protocol (markers 0x10..0x13, 3 bytes per escape: marker + payload
// (low, high)); files on disk and everything the engine did NOT get from
// the mod pipeline are real UTF-8. The save system needs both directions
// at the engine<->filesystem boundary.
//
// Protocol (constants verbatim from upstream; decode side already lives
// in font.cpp for glyph fetching):
//   0x10: id = (high<<8)|low                     (raw)
//   0x11: id = ... - 0xE                         (encode: low += 14)
//   0x12: id = ... + 0x900                       (encode: high -= 9)
//   0x13: id = ... + 0x8F2                       (encode: both)
//   dead zone: id in (0x100, 0x98F) or > 0xFFFF -> U+2026
//   encode-side marker choice: special-byte collision table (engine
//   meta characters); code points in (0x100, 0xA00) are shifted into the
//   private use area (+0xE000) BEFORE the high/low split.
//
// Engine CString == libstdc++ std::__cxx11::basic_string byte-for-byte
// (verified: {data@+0, len@+8, SSO buf@+0x10}, SSO iff data == this+0x10;
// the engine's own dtor does the same conditional operator delete). The
// cstr_* helpers therefore treat engine objects as std::string and rely
// on both allocators bottoming out in glibc malloc (cross new/delete is
// safe on Linux; the render module's append_byte has run this contract
// since M3).

#include <cstddef>
#include <cstdint>
#include <string>

namespace eu4cjk::escape {

// True if the byte range contains an escape marker (0x10..0x13). Cheap
// pre-filter used to leave vanilla strings untouched.
bool has_escapes(const char* s, size_t n);

// True if the range contains at least one multi-byte UTF-8 sequence and
// decodes as valid UTF-8 (used by the read direction so escaped junk
// never gets double-converted).
bool is_valid_utf8_multibyte(const char* s, size_t n);

// escaped byte stream -> UTF-8 (upstream convertEscapedTextToWideText +
// convertWideTextToUtf8). Non-escape bytes map through the CP1252 table.
std::string to_utf8(const char* s, size_t n);

// UTF-8 -> escaped byte stream (upstream convertTextToWideText +
// convertWideTextToEscaped). ASCII (<0x80) passes through unchanged.
// Code points above the BMP are split into surrogate pairs, matching
// upstream's 16-bit wchar pipeline exactly.
std::string to_escaped(const char* s, size_t n);

// In-place rewrite of an engine CString. Return true if a conversion
// happened, false if the string was left untouched (no escapes for
// cstr_to_utf8; no valid multibyte UTF-8 for cstr_to_escaped).
bool cstr_to_utf8(void* engine_cstring);
bool cstr_to_escaped(void* engine_cstring);

} // namespace eu4cjk::escape
