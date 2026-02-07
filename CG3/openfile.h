#pragma once
#ifdef _WIN32
#include <windows.h>
inline void openImage(const char* name) {
    ShellExecuteA(NULL, "open", name, NULL, NULL, SW_SHOW);
}
#else
inline void openImage(const char*) {}
#endif
