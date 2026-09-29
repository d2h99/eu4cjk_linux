#pragma once

namespace eu4cjk::version {

enum GameVersion {
    UNKNOWN = 0,
    v1_37_0_0 = 1,
};

extern GameVersion g_detected;

GameVersion detect();
const char* name(GameVersion v);

}
