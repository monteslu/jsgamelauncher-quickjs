/*
 * Platform portability implementations. See platform.h for the rationale.
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
  #include <mmsystem.h>   /* timeBeginPeriod */
#else
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <errno.h>
  #ifdef __APPLE__
    #include <mach-o/dyld.h>
  #endif
#endif

/* ----------------------------------------------------------- executable path */

bool jsglq_exe_path(char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = 0;

#if defined(_WIN32)
    wchar_t wide[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, wide, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    /* Convert to UTF-8: paths with non-ASCII characters are ordinary on Windows
       and a lossy conversion would break exactly those users. */
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)out_size, NULL, NULL);
    return len > 0;

#elif defined(__APPLE__)
    uint32_t size = (uint32_t)out_size;
    if (_NSGetExecutablePath(out, &size) != 0) return false;
    /* _NSGetExecutablePath may return a path containing symlinks or `..`, so
       canonicalize it — the result is used to locate files beside the binary. */
    char resolved[4096];
    if (realpath(out, resolved)) {
        if (strlen(resolved) >= out_size) return false;
        strcpy(out, resolved);
    }
    return true;

#else
    ssize_t n = readlink("/proc/self/exe", out, out_size - 1);
    if (n <= 0) return false;
    out[n] = 0;
    return true;
#endif
}

/* ------------------------------------------------------------- path splitting */

/*
 * Both accept either separator on Windows: a path can arrive from the command
 * line with forward slashes, and treating only backslash as a separator would
 * return the whole string as the "base name".
 */
static const char *last_separator(const char *path)
{
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *back = strrchr(path, '\\');
    if (!slash || (back && back > slash)) slash = back;
#endif
    return slash;
}

void jsglq_dirname(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    const char *slash = path ? last_separator(path) : NULL;
    if (!slash) {
        snprintf(out, out_size, ".");
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len == 0) {                      /* "/foo" -> "/" */
        snprintf(out, out_size, JSGLQ_PATH_SEP_STR);
        return;
    }
    if (len >= out_size) len = out_size - 1;
    memcpy(out, path, len);
    out[len] = 0;
}

void jsglq_basename(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (!path || !*path) { snprintf(out, out_size, "."); return; }

    /* Ignore a trailing separator so "/games/pong/" gives "pong", not "". */
    char trimmed[4096];
    snprintf(trimmed, sizeof(trimmed), "%s", path);
    size_t tlen = strlen(trimmed);
    while (tlen > 1 && (trimmed[tlen - 1] == '/'
#ifdef _WIN32
                        || trimmed[tlen - 1] == '\\'
#endif
        )) {
        trimmed[--tlen] = 0;
    }

    const char *slash = last_separator(trimmed);
    snprintf(out, out_size, "%s", slash ? slash + 1 : trimmed);
}

/* ------------------------------------------------------------------- fs calls */

bool jsglq_mkdir(const char *path)
{
#ifdef _WIN32
    if (_mkdir(path) == 0) return true;
    return errno == EEXIST;
#else
    if (mkdir(path, 0755) == 0) return true;
    return errno == EEXIST;
#endif
}

bool jsglq_mkdir_p(const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != JSGLQ_PATH_SEP) continue;
        char saved = *p;
        *p = 0;
        if (tmp[0] && !jsglq_mkdir(tmp)) { *p = saved; return false; }
        *p = saved;
    }
    return jsglq_mkdir(tmp);
}

bool jsglq_realpath(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
#ifdef _WIN32
    DWORD n = GetFullPathNameA(path, (DWORD)out_size, out, NULL);
    if (n == 0 || n >= out_size) return false;
    /* GetFullPathName does not verify existence, which realpath() does, and
       callers rely on that check to reject paths outside the game directory. */
    return _access(out, 0) == 0;
#else
    char resolved[4096];
    if (!realpath(path, resolved)) return false;
    if (strlen(resolved) >= out_size) return false;
    strcpy(out, resolved);
    return true;
#endif
}

bool jsglq_is_file(const char *path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool jsglq_is_dir(const char *path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

void jsglq_temp_dir(char *out, size_t out_size)
{
#ifdef _WIN32
    DWORD n = GetTempPathA((DWORD)out_size, out);
    if (n == 0) { snprintf(out, out_size, "."); return; }
    /* GetTempPath always appends a separator; strip it so callers can join
       consistently on every platform. */
    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\\' || out[len - 1] == '/')) out[--len] = 0;
#else
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(out, out_size, "%s", tmp);
#endif
}

bool jsglq_join_path(char *out, size_t out_size, const char *dir, const char *leaf)
{
    if (!out || out_size == 0) return false;
    out[0] = 0;
    if (!dir || !leaf) return false;

    size_t dlen = strlen(dir);
    /* Do not double the separator when the directory already ends in one. */
    int need_sep = (dlen > 0 && dir[dlen - 1] != '/' && dir[dlen - 1] != JSGLQ_PATH_SEP);
    size_t total = dlen + (size_t)(need_sep ? 1 : 0) + strlen(leaf);
    if (total + 1 > out_size) return false;

    memcpy(out, dir, dlen);
    size_t at = dlen;
    if (need_sep) out[at++] = '/';
    strcpy(out + at, leaf);
    return true;
}

void jsglq_setenv(const char *name, const char *value)
{
#ifdef _WIN32
    char buf[8192];
    snprintf(buf, sizeof(buf), "%s=%s", name, value);
    _putenv(buf);
#else
    setenv(name, value, 1);
#endif
}

/* ------------------------------------------------------------------- clock --- */

double jsglq_monotonic_ms(void)
{
#ifdef _WIN32
    /* QueryPerformanceCounter rather than GetTickCount64: the latter is quantized
       to the ~15ms scheduler tick, which is coarser than a frame. */
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

void jsglq_sleep_ms(double ms)
{
    if (ms <= 0) return;
#ifdef _WIN32
    /* Sleep() honours only whole milliseconds and, by default, rounds up to the
       scheduler tick. timeBeginPeriod(1) is what makes a 1ms request behave like
       one; without it short frame-pacing sleeps overshoot badly. */
    static int period_set = 0;
    if (!period_set) { timeBeginPeriod(1); period_set = 1; }
    DWORD whole = (DWORD)ms;
    if (whole == 0) whole = 1;
    Sleep(whole);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1e6);
    nanosleep(&ts, NULL);
#endif
}
