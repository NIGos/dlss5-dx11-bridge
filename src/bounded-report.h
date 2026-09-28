#pragma once
#include <cstddef>
#include <cstring>

// Keep a valid prefix without invoking the CRT invalid-parameter handler.
template <std::size_t N>
bool AppendCrashReport(char (&dst)[N], const char *src)
{
    static_assert(N > 0, "A terminator is required");
    const std::size_t used = std::strlen(dst);
    const std::size_t length = std::strlen(src);
    const std::size_t room = N - 1 - used;
    const std::size_t count = length < room ? length : room;
    std::memcpy(dst + used, src, count);
    dst[used + count] = '\0';
    return count == length;
}
