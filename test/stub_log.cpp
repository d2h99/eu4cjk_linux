#include "../src/log.h"

#include <cstdarg>
#include <cstdio>

namespace eu4cjk {

void log_line(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

}
