#include "version.h"
#include "log.h"
#include "sha256.h"
#include "byte_pattern.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace eu4cjk::version {

GameVersion g_detected = UNKNOWN;

namespace {

constexpr char kExpectedSha256[] =
    "af115d3b0e54a05eca0198ed569db90ca225728afda03b5ac4ded251520a7ce3";
constexpr uint32_t kExpectedSteamBuildId = 15918133;

void self_check_exe()
{
    char hex[65];
    {
        Sha256 s;
        int fd = open("/proc/self/exe", O_RDONLY);
        if (fd < 0) {
            log_line("[eu4cjk] sha256 self-check: cannot open /proc/self/exe\n");
            return;
        }
        char buf[1 << 16];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) s.update(buf, static_cast<size_t>(n));
        close(fd);
        uint8_t digest[32];
        s.final(digest);
        static const char hc[] = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) {
            hex[2 * i] = hc[digest[i] >> 4];
            hex[2 * i + 1] = hc[digest[i] & 0xF];
        }
        hex[64] = '\0';
    }
    if (std::strcmp(hex, kExpectedSha256) == 0)
        log_line("[eu4cjk] sha256 self-check: MATCH (expect Steam buildid %u)\n",
                 static_cast<unsigned>(kExpectedSteamBuildId));
    else
        log_line("[eu4cjk] sha256 self-check: MISMATCH got %.16s want %.16s"
                 " (buildid %u) - patterns may fail!\n",
                 hex, kExpectedSha256, static_cast<unsigned>(kExpectedSteamBuildId));
}

}

const char* name(GameVersion v)
{
    switch (v) {
    case v1_37_0_0: return "v1_37_0_0";
    case UNKNOWN: break;
    }
    return "UNKNOWN";
}

GameVersion detect()
{
    auto& bp = BytePattern::temp_instance();

    bp.find_pattern("45 55 34 20 76 31 2E ? ? 2E ?");
    if (bp.count() < 1) {
        g_detected = UNKNOWN;
        log_line("[eu4cjk] version: NOT RECOGNIZED (marker not found), no patches applied\n");
        return UNKNOWN;
    }

    const char* s = reinterpret_cast<const char*>(bp.get_first().address());
    if (s[7] == '3' && s[8] == '7' && s[10] == '5') {
        g_detected = v1_37_0_0;
        log_line("[eu4cjk] version: 1.37.5.0 @ 0x%lx [%s]\n",
                 static_cast<unsigned long>(bp.get_first().address()), name(g_detected));
    } else {
        g_detected = UNKNOWN;
        log_line("[eu4cjk] version: NOT RECOGNIZED (1.%c%c.%c), no patches applied\n",
                 s[7], s[8], s[10]);
        return UNKNOWN;
    }

    self_check_exe();
    return g_detected;
}

}
