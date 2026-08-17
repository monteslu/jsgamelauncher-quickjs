/*
 * Platform portability: the handful of OS calls this host needs.
 *
 * Everything else in the codebase is portable C/C++ already. These are the calls
 * with no common spelling across Linux, macOS and Windows:
 *
 *   executable path   /proc/self/exe   _NSGetExecutablePath   GetModuleFileNameW
 *   directory name    dirname()        dirname()              _splitpath
 *   base name         basename()       basename()             _splitpath
 *   mkdir             mkdir(path,mode) mkdir(path,mode)       _mkdir(path)
 *   realpath          realpath()       realpath()             GetFullPathNameW
 *
 * Note that dirname()/basename() are not merely absent on Windows — the POSIX
 * versions are also allowed to MODIFY their argument and may return a pointer to
 * static storage, so the portable wrappers here take an explicit output buffer.
 * That removes a footgun rather than just papering over a missing header.
 */
#ifndef JSGLQ_PLATFORM_H
#define JSGLQ_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Absolute path of the running executable. Returns false if it cannot be found,
   which callers must handle: it is how a fused binary locates its own payload. */
bool jsglq_exe_path(char *out, size_t out_size);

/* Directory portion of `path` into `out`. Never modifies `path`. */
void jsglq_dirname(const char *path, char *out, size_t out_size);

/* Final component of `path` into `out`. Never modifies `path`. */
void jsglq_basename(const char *path, char *out, size_t out_size);

/* Create one directory. Succeeds if it already exists. */
bool jsglq_mkdir(const char *path);

/* Create a directory and any missing parents. */
bool jsglq_mkdir_p(const char *path);

/* Canonical absolute path. Returns false when the path does not exist. */
bool jsglq_realpath(const char *path, char *out, size_t out_size);

/* True if `path` exists and is a regular file / a directory. */
bool jsglq_is_file(const char *path);
bool jsglq_is_dir(const char *path);

/* The OS temp directory, without a trailing separator. */
void jsglq_temp_dir(char *out, size_t out_size);

/* Join two path components into `out`.
 *
 * Returns false if the result would not fit, WITHOUT writing a truncated path.
 * That distinction matters: snprintf truncates silently, and a truncated path
 * is a lookup against the wrong file rather than an error — so a caller that
 * ignores the return value gets a clean failure instead of a mystery. */
bool jsglq_join_path(char *out, size_t out_size, const char *dir, const char *leaf);

/* Set an environment variable for this process (used to pass the fused runtime
   directory to the module loader). */
void jsglq_setenv(const char *name, const char *value);

/* Milliseconds from an unspecified monotonic origin. Monotonic matters: frame
   pacing and setTimeout must not jump when the wall clock is adjusted. */
double jsglq_monotonic_ms(void);

/* Sleep for the given milliseconds. Sub-millisecond values are honoured where
   the platform allows; a value <= 0 returns immediately. */
void jsglq_sleep_ms(double ms);

/* The platform's path separator, for building paths that the OS will accept. */
#ifdef _WIN32
#define JSGLQ_PATH_SEP '\\'
#define JSGLQ_PATH_SEP_STR "\\"
#else
#define JSGLQ_PATH_SEP '/'
#define JSGLQ_PATH_SEP_STR "/"
#endif

#ifdef __cplusplus
}
#endif

#endif /* JSGLQ_PLATFORM_H */
