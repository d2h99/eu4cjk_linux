#include "../src/byte_pattern.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

// Usage: repro <path-to-eu4-binary> [offset hex]
// (offset defaults to the v1.37.5 version-string window)
#ifndef EU4_OFF
#define EU4_OFF 0x21d4000
#endif

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <eu4-binary-path>\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    const size_t kLen = 0x2000;
    const off_t kOff = EU4_OFF;

    void* m = mmap(nullptr, kLen, PROT_READ, MAP_PRIVATE, fd, kOff);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }

    uintptr_t str = reinterpret_cast<uintptr_t>(m) + (0x21d4b61 - kOff);
    printf("map=%p string=%p '%.19s'\n", m, reinterpret_cast<void*>(str),
        reinterpret_cast<const char*>(str));

    auto& bp = BytePattern::temp_instance();
    bp.set_range(memory_pointer(reinterpret_cast<uintptr_t>(m)),
                 memory_pointer(reinterpret_cast<uintptr_t>(m) + kLen));
    bp.set_pattern("45 55 34 20 76 20 31 2E 33 37 2E 35 2E 30 20 49 6E 63 61").search();
    printf("window hits: %zu\n", bp.count());

    return 0;
}
