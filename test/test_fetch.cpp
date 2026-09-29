#include "selftest_h.h"

// fetch_glyph decoder unit tests + per-font external-table isolation
// (M3-T2/T3).

namespace eu4cjk_test {

void run_fetch_tests()
{
    std::memset(g_font, 0, sizeof(g_font));
    std::memset(g_ext, 0, sizeof(g_ext));

    int marker_latin = 0, marker_ext = 0, marker_nf = 0, marker_2026 = 0;
    *reinterpret_cast<void**>(g_font + 0x100 + 0x41 * 8) = &marker_latin; // 'A'
    g_ext[0x4E2D] = &marker_ext;
    g_ext[0x4E20] = &marker_ext;
    g_ext[0x20A4] = &marker_ext;
    g_ext[0x2026] = &marker_2026;

    fetch_case("plain byte -> inline slot", "A", 1, &marker_latin);
    fetch_case("missing slot is null", "Z", 1, nullptr);
    fetch_case("prefix 0x10 raw id", "\x10\x2D\x4E", 3, &marker_ext);      // 0x4E2D
    fetch_case("prefix 0x11 (-0xE)", "\x11\x2E\x4E", 3, &marker_ext);      // 0x4E20
    fetch_case("prefix 0x12 (+0x900)", "\x12\x26\x17", 3, &marker_2026);   // 0x2026
    fetch_case("prefix 0x13 (+0x8F2)", "\x13\xB2\x17", 3, &marker_ext);    // 0x20A4
    fetch_case("dead zone -> NOT_DEF", "\x10\x00\x02", 3, &marker_2026);   // 0x200 -> 0x2026

    g_consumed = 0;
    const void* got_null = eu4cjk::font::fetch_glyph(g_font, nullptr, "\x10\x2D\x4E", &g_consumed);
    check(got_null == nullptr && g_consumed == 3, "fetch_glyph: external null table");

    g_consumed = 0;
    check(eu4cjk::font::fetch_glyph(nullptr, nullptr, "A", &g_consumed) == nullptr
              && g_consumed == 1,
          "fetch_glyph: null font tolerated");
    (void)marker_nf;
}

void run_gate_tests()
{
    // Fake font pointers: only the escape path (id > 0xFF) touches them, and
    // that path never dereferences the font, only compares it.
    const void* fontA = reinterpret_cast<const void*>(0x11110000ull);
    const void* fontB = reinterpret_cast<const void*>(0x22220000ull);
    const void* fontC = reinterpret_cast<const void*>(0x33330000ull);

    eu4cjk::font::GateArgs a{};
    a.id = 0x4E1C; // "\x10\x1C\x4E"
    a.font = fontA;
    for (int i = 0; i < 7; ++i) a.f[i] = static_cast<uint16_t>(0x1000 + i);
    int rc = eu4cjk_glyph_gate(&a);
    a.font = fontB;
    for (int i = 0; i < 7; ++i) a.f[i] = static_cast<uint16_t>(0x2000 + i);
    rc &= eu4cjk_glyph_gate(&a);
    check(rc == 1 && eu4cjk::font::font_registry_count() == 2,
          "gate: two fonts registered separately");

    uint32_t consumed = 0;
    const uint16_t* na = static_cast<const uint16_t*>(
        eu4cjk::font::fetch_glyph_live(fontA, "\x10\x1C\x4E", &consumed));
    const uint16_t* nb = static_cast<const uint16_t*>(
        eu4cjk::font::fetch_glyph_live(fontB, "\x10\x1C\x4E", &consumed));
    check(consumed == 3 && na && nb && na != nb && na[3] == 0x1003 && nb[3] == 0x2003,
          "gate: same id, per-font nodes isolated");

    a.font = fontA; // duplicate within same font -> keep-first
    a.f[0] = 0xBEEF;
    eu4cjk_glyph_gate(&a);
    const uint16_t* na2 = static_cast<const uint16_t*>(
        eu4cjk::font::fetch_glyph_live(fontA, "\x10\x1C\x4E", &consumed));
    check(na2 == na && na2[0] == 0x1000, "gate: keep-first per font");

    check(eu4cjk::font::fetch_glyph_live(fontC, "\x10\x1C\x4E", &consumed) == nullptr,
          "gate: unregistered font -> null (no alloc on render path)");
}

} // namespace eu4cjk_test
