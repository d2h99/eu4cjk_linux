#include "selftest_h.h"

// Injector + byte_pattern live tests and the runner main().

namespace {

__attribute__((noinline)) int target_fn() { return 1; }
__attribute__((noinline)) int hook_fn() { return 42; }

int (*volatile g_fp)() = target_fn;

} // namespace

namespace eu4cjk_test {

int g_fails = 0;

} // namespace eu4cjk_test

int main()


{
    auto& bp = BytePattern::temp_instance();

    printf("diag: kMarker @ %p (len %zu)\n", kMarker, sizeof(kMarker));

    bp.find_pattern("45 55 34 43 4A 4B 21 53");
    check(!bp.error() && bp.count() >= 1, "diag: 'EU4CJK!S' (kMarker head) hit");

    bp.find_pattern("4D 41 52 4B 45 52 24 30 31");
    check(!bp.error() && bp.count() >= 1, "byte_pattern: exact bytes 'MARKER$01' hit");
    {
        bool in_marker = false;
        bp.for_each_result([&](memory_pointer p) {
            if (within_marker(p.address())
                && memcmp(reinterpret_cast<const void*>(p.address()), "MARKER$01", 9) == 0)
                in_marker = true;
        });
        check(in_marker, "byte_pattern: exact hit points into marker string");
    }

    bp.find_pattern("4D 4? 52 4B");
    check(!bp.error() && bp.count() >= 1, "byte_pattern: nibble wildcard '4D 4? 52 4B' hit");
    {
        bool at_mark = false;
        bp.for_each_result([&](memory_pointer p) {
            if (memcmp(reinterpret_cast<const void*>(p.address()), "MARK", 4) == 0
                && within_marker(p.address()))
                at_mark = true;
        });
        check(at_mark, "byte_pattern: nibble wildcard hit address correct");
    }

    bp.find_pattern("53 45 4C 46 54 ?? 52 4D");
    check(!bp.error() && bp.count() >= 1, "byte_pattern: full-byte wildcard hit");
    {
        bool at_selfterm = false;
        bp.for_each_result([&](memory_pointer p) {
            if (memcmp(reinterpret_cast<const void*>(p.address()), "SELFTERM", 8) == 0
                && within_marker(p.address()))
                at_selfterm = true;
        });
        check(at_selfterm, "byte_pattern: full-byte wildcard hit address correct");
    };

    static uint8_t buf[8] = {0};
    uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t dst[8] = {0};
    Injector::WriteMemoryRaw(buf, src, 8, true);
    Injector::ReadMemoryRaw(buf, dst, 8, true);
    check(memcmp(src, dst, 8) == 0, "injector: WriteMemoryRaw/ReadMemoryRaw roundtrip");

    check(g_fp() == 1, "injector: target returns 1 before hook");
    Injector::MakeJMP(reinterpret_cast<void*>(target_fn), reinterpret_cast<void*>(hook_fn));
    uint8_t b0 = Injector::ReadMemory<uint8_t>(reinterpret_cast<void*>(target_fn));
    uint8_t b1 = Injector::ReadMemory<uint8_t>(reinterpret_cast<uintptr_t>(target_fn) + 1);
    check(b0 == 0xE9 || (b0 == 0xFF && b1 == 0x25), "injector: E9/FF25 branch bytes written");
    check(g_fp() == 42, "injector: MakeJMP detours target to hook");

    run_fetch_tests();
    run_gate_tests();
    run_stub_tests();
    run_full_stub_tests();
    run_measure_stub_tests();
    run_wrap_stub_tests();
    run_ct_tdiv_tests();
    run_ct_single_tests();
    run_escape_tests();

    printf("%s: %d failure(s)\n", g_fails == 0 ? "SELFTEST PASS" : "SELFTEST FAIL", g_fails);
    return g_fails;
}
