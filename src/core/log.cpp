#include "log.h"
#include <cstdio>
#include <mutex>
#include <chrono>
#ifdef _WIN32
#include <filesystem>
#include <windows.h>
#endif
// Android: stderr goes nowhere on an app process, so the log would be lost; every line also goes
// to logcat (adb logcat -s scacelith).
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace logx {
static std::mutex g_mutex;
static FILE* g_file = nullptr;
static auto g_start = std::chrono::steady_clock::now();

// The path is UTF-8: opened as a wide path on Windows (u8path), whatever the process code page.
bool init(const char* path) {
    std::lock_guard<std::mutex> lk(g_mutex);
#ifdef _WIN32
    if (path) g_file = _wfopen(std::filesystem::u8path(path).c_str(), L"w");
#else
    if (path) g_file = std::fopen(path, "w");
#endif
    return g_file != nullptr;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

void vwrite(Level lvl, const char* fmt, va_list ap) {
    static const char* tags[] = {"D", "I", "W", "E"};
    char msg[4096];
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    char line[4200];
    std::snprintf(line, sizeof(line), "[%8.3f %s] %s\n", t, tags[int(lvl)], msg);
    std::lock_guard<std::mutex> lk(g_mutex);
    std::fputs(line, stderr);
    if (g_file) { std::fputs(line, g_file); std::fflush(g_file); }
#ifdef _WIN32
    OutputDebugStringA(line);
#endif
#ifdef __ANDROID__
    static const int prio[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    __android_log_print(prio[int(lvl)], "scacelith", "%s", msg);
#endif
}

void write(Level lvl, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vwrite(lvl, fmt, ap);
    va_end(ap);
}
}  // namespace logx
