#include "escape.h"

#include <cstring>

namespace eu4cjk::escape {
namespace {

// CP1252 <-> UCS2 for the 0x80..0x9F block (upstream UCS2ToCP1252 /
// CP1252ToUCS2, entries verbatim). Everything else is identity.
struct CpMap { uint16_t ucs2; uint8_t cp1252; };
constexpr CpMap kCpMap[] = {
    {0x20AC, 0x80}, {0x201A, 0x82}, {0x0192, 0x83}, {0x201E, 0x84},
    {0x2026, 0x85}, {0x2020, 0x86}, {0x2021, 0x87}, {0x02C6, 0x88},
    {0x2030, 0x89}, {0x0160, 0x8A}, {0x2039, 0x8B}, {0x0152, 0x8C},
    {0x017D, 0x8E}, {0x2018, 0x91}, {0x2019, 0x92}, {0x201C, 0x93},
    {0x201D, 0x94}, {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97},
    {0x02DC, 0x98}, {0x2122, 0x99}, {0x0161, 0x9A}, {0x203A, 0x9B},
    {0x0153, 0x9C}, {0x017E, 0x9E}, {0x0178, 0x9F},
};

// Upstream returns wchar_t (16-bit): identity for everything outside
// the table - do NOT truncate to the cp1252 byte, the encode-side
// inequality test depends on the full 16-bit compare.
uint16_t ucs2_to_cp1252(uint16_t cp)
{
    for (const auto& m : kCpMap)
        if (m.ucs2 == cp) return m.cp1252;
    return cp;
}

uint16_t cp1252_to_ucs2(uint8_t b)
{
    for (const auto& m : kCpMap)
        if (m.cp1252 == b) return m.ucs2;
    return b;
}

// Upstream's special-byte set: any engine metacharacter appearing in
// either payload byte forces a biased marker so the payload never
// collides with the meta character itself.
bool is_special_byte(uint8_t b)
{
    switch (b) {
    case 0xA4: case 0xA3: case 0xA7: case 0x24: case 0x5B: case 0x00:
    case 0x5C: case 0x20: case 0x0D: case 0x0A: case 0x22: case 0x7B:
    case 0x7D: case 0x40: case 0x80: case 0x7E: case 0x2F: case 0x5F:
    case 0xBD: case 0x3B: case 0x5D: case 0x3D: case 0x23: case 0x3F:
    case 0x3A: case 0x3C: case 0x3E: case 0x2A: case 0x7C:
        return true;
    default:
        return false;
    }
}

// Decode one UTF-8 sequence. Returns the code point and advances *i;
// 0xFFFFFFFF on malformed input (caller substitutes U+2026, matching the
// dead-zone policy).
uint32_t utf8_decode_one(const char* s, size_t n, size_t& i)
{
    uint8_t b0 = static_cast<uint8_t>(s[i]);
    size_t len;
    uint32_t cp;
    if (b0 < 0x80) { ++i; return b0; }
    else if ((b0 & 0xE0) == 0xC0) { len = 2; cp = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07; }
    else { ++i; return 0xFFFFFFFF; }
    if (i + len > n) { ++i; return 0xFFFFFFFF; }
    for (size_t k = 1; k < len; ++k) {
        uint8_t bk = static_cast<uint8_t>(s[i + k]);
        if ((bk & 0xC0) != 0x80) { i += 1; return 0xFFFFFFFF; }
        cp = (cp << 6) | (bk & 0x3F);
    }
    i += len;
    return cp;
}

void utf8_encode_one(uint32_t cp, std::string& out)
{
    if (cp > 0x10FFFF) cp = 0x2026;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// One 16-bit unit of the upstream encode pipeline (wchar_t on Win).
void encode_unit(uint16_t unit, std::string& out)
{
    // CP1252-block specials pass as a single byte: upstream writes
    // (BYTE)cp - the LOW BYTE OF THE ORIGINAL code point, not the cp1252
    // mapping (upstream quirk; port verbatim for byte compatibility).
    if (ucs2_to_cp1252(unit) != unit) {
        out.push_back(static_cast<char>(static_cast<uint8_t>(unit)));
        return;
    }
    uint32_t cp = unit;
    if (cp > 0x100 && cp < 0xA00) cp += 0xE000;   // PUA shift
    uint8_t low = static_cast<uint8_t>(cp & 0xFF);
    uint8_t high = static_cast<uint8_t>((cp >> 8) & 0xFF);
    if (high == 0) {                               // latin1-range
        out.push_back(static_cast<char>(low));
        return;
    }
    uint8_t marker = 0x10;
    if (is_special_byte(high)) marker += 2;
    if (is_special_byte(low)) marker += 1;
    switch (marker) {
    case 0x11: low += 14; break;
    case 0x12: high -= 9; break;
    case 0x13: low += 14; high -= 9; break;
    default: break;
    }
    out.push_back(static_cast<char>(marker));
    out.push_back(static_cast<char>(low));
    out.push_back(static_cast<char>(high));
}

// One escape triple / raw byte of the upstream decode pipeline; appends
// 16-bit units (surrogates kept for later recombination, like the Win
// wstring intermediate).
void decode_units(const char* s, size_t n, std::u16string& units)
{
    size_t i = 0;
    while (i < n) {
        uint8_t b = static_cast<uint8_t>(s[i]);
        if (b >= 0x10 && b <= 0x13 && i + 2 < n) {
            uint8_t low = static_cast<uint8_t>(s[i + 1]);
            uint8_t high = static_cast<uint8_t>(s[i + 2]);
            uint32_t sp = (static_cast<uint32_t>(high) << 8) | low;
            if (b == 0x11) sp -= 0xE;
            else if (b == 0x12) sp += 0x900;
            else if (b == 0x13) sp += 0x8F2;
            i += 3;
            if (sp > 0xFFFF || (sp < 0x98F && sp > 0x100)) sp = 0x2026;
            if (sp > 0xFFFF) sp = 0x2026;          // bias overflow guard
            units.push_back(static_cast<char16_t>(sp));
        } else {
            units.push_back(static_cast<char16_t>(cp1252_to_ucs2(b)));
            ++i;
        }
    }
}

} // namespace

bool has_escapes(const char* s, size_t n)
{
    for (size_t i = 0; i + 2 < n; ++i) {
        uint8_t b = static_cast<uint8_t>(s[i]);
        if (b >= 0x10 && b <= 0x13) return true;
    }
    return false;
}

bool is_valid_utf8_multibyte(const char* s, size_t n)
{
    bool multibyte = false;
    size_t i = 0;
    while (i < n) {
        uint8_t b = static_cast<uint8_t>(s[i]);
        if (b < 0x80) { ++i; continue; }
        uint32_t cp = utf8_decode_one(s, n, i);
        if (cp == 0xFFFFFFFF) return false;
        multibyte = true;
    }
    return multibyte;
}

std::string to_utf8(const char* s, size_t n)
{
    std::u16string units;
    units.reserve(n);
    decode_units(s, n, units);
    std::string out;
    out.reserve(units.size() * 3);
    size_t i = 0;
    while (i < units.size()) {
        char16_t u = units[i];
        uint32_t cp;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units.size()
            && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((static_cast<uint32_t>(u) - 0xD800) << 10)
                 + (static_cast<uint32_t>(units[i + 1]) - 0xDC00);
            i += 2;
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            cp = 0x2026;                            // unpaired surrogate
            i += 1;
        } else {
            cp = u;
            i += 1;
        }
        utf8_encode_one(cp, out);
    }
    return out;
}

std::string to_escaped(const char* s, size_t n)
{
    std::string out;
    out.reserve(n * 3 + 8);
    size_t i = 0;
    while (i < n) {
        uint32_t cp = utf8_decode_one(s, n, i);
        if (cp == 0xFFFFFFFF) cp = 0x2026;
        if (cp > 0xFFFF) {
            // surrogate pair, exactly like upstream's 16-bit pipeline
            cp -= 0x10000;
            encode_unit(static_cast<uint16_t>(0xD800 + (cp >> 10)), out);
            encode_unit(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)), out);
        } else {
            encode_unit(static_cast<uint16_t>(cp), out);
        }
    }
    return out;
}

bool cstr_to_utf8(void* engine_cstring)
{
    auto* str = static_cast<std::string*>(engine_cstring);
    const char* d = str->data();
    size_t n = str->size();
    if (!has_escapes(d, n)) return false;
    *str = to_utf8(d, n);
    return true;
}

bool cstr_to_escaped(void* engine_cstring)
{
    auto* str = static_cast<std::string*>(engine_cstring);
    const char* d = str->data();
    size_t n = str->size();
    if (!is_valid_utf8_multibyte(d, n)) return false;
    if (has_escapes(d, n)) return false;   // already escaped; never touch
    *str = to_escaped(d, n);
    return true;
}

} // namespace eu4cjk::escape
