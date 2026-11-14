// setsquare shim: minimal stand-in for FreeCAD's Base/Console.h.
// Solver log chatter is dropped; warnings go to stderr.
#pragma once

#include <cstdio>
#include <format>
#include <string>

namespace Base
{

class ConsoleSingleton
{
public:
    template<typename... Args>
    void log(std::format_string<Args...>, Args&&...)
    {}

    template<typename... Args>
    void warning(std::format_string<Args...> fmt, Args&&... args)
    {
        std::string s = std::format(fmt, std::forward<Args>(args)...);
        std::fputs(s.c_str(), stderr);
    }

    template<typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args)
    {
        std::string s = std::format(fmt, std::forward<Args>(args)...);
        std::fputs(s.c_str(), stderr);
    }
};

inline ConsoleSingleton& Console()
{
    static ConsoleSingleton instance;
    return instance;
}

}  // namespace Base
