// xr.ages — Logausgabe der XR-Schicht: auf der Quest nach logcat (Tag "xr.ages"), am PC (Host-Tests,
// Desktop-Vorschau) nach stderr.
#pragma once

#ifdef __ANDROID__
#include <android/log.h>
#define XLOGI(...) __android_log_print(ANDROID_LOG_INFO, "xr.ages", __VA_ARGS__)
#define XLOGW(...) __android_log_print(ANDROID_LOG_WARN, "xr.ages", __VA_ARGS__)
#define XLOGE(...) __android_log_print(ANDROID_LOG_ERROR, "xr.ages", __VA_ARGS__)
#else
#include <cstdio>
#define XLOG_HOST_(level, ...)                         \
    do {                                               \
        std::fprintf(stderr, "[xr.ages] " level " "); \
        std::fprintf(stderr, __VA_ARGS__);             \
        std::fputc('\n', stderr);                      \
    } while (0)
#define XLOGI(...) XLOG_HOST_("I", __VA_ARGS__)
#define XLOGW(...) XLOG_HOST_("W", __VA_ARGS__)
#define XLOGE(...) XLOG_HOST_("E", __VA_ARGS__)
#endif
