#include "../src/bounded-report.h"
#include <cassert>
#include <cstdio>
#include <string>

int main()
{
    char text[2048] = {};
    assert(AppendCrashReport(text, "hello"));
    assert(std::strcmp(text, "hello") == 0);
    std::string large(2108, 'x');
    assert(!AppendCrashReport(text, large.c_str()));
    assert(std::strlen(text) == 2047);
    assert(!AppendCrashReport(text, "one more line"));
    char tiny[1] = {};
    assert(!AppendCrashReport(tiny, "x") && tiny[0] == 0);
    puts("bounded crash-report test passed");
}
