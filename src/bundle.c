/*
 * Fused-binary support: run a game embedded in the executable itself.
 *
 * The bundle is appended to a copy of the launcher and located by a trailer at the
 * very end of the file:
 *
 *     [ launcher ELF/PE/Mach-O ][ zip payload ][ 32-byte trailer ]
 *
 * WHY A TRAILER AND NOT A LINKER SECTION:
 *
 * Deno-style section injection is the better answer on macOS specifically, because
 * appending data after the Mach-O breaks a code signature while a real section can
 * be signed over. But section injection needs a per-format object-file editor, and
 * it also means the *fuser* must understand ELF, PE, and Mach-O.
 *
 * A trailer works identically on all three formats with no object-file knowledge,
 * and matches the model LOVE has shipped for years. The macOS signing caveat is
 * real and is documented in scripts/fuse.mjs: sign AFTER fusing, not before.
 *
 * The payload is a flat archive (see scripts/fuse.mjs), not a zip: the launcher
 * would otherwise need a zip decoder in C for no benefit, since the fuser and the
 * launcher ship together and the payload never travels between different versions.
 */
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

#define JSGLQ_MAGIC "JSGLQBND"     /* 8 bytes */
#define TRAILER_SIZE 32            /* magic(8) + version(4) + offset(8) + size(8) + crc(4) */

typedef struct {
    uint64_t offset;
    uint64_t size;
    uint32_t crc;
    uint32_t version;
} BundleInfo;

static uint32_t read_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64le(const uint8_t *p)
{
    return (uint64_t)read_u32le(p) | ((uint64_t)read_u32le(p + 4) << 32);
}

/*
 * Look for a bundle trailer at the end of our own executable.
 * Returns true only if the trailer is present AND self-consistent: a launcher that
 * happens to end in the right 8 bytes must not be treated as fused.
 */
bool jsglq_bundle_find(const char *exe_path, BundleInfo *out)
{
    FILE *f = fopen(exe_path, "rb");
    if (!f) return false;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long file_size = ftell(f);
    if (file_size < TRAILER_SIZE) { fclose(f); return false; }

    if (fseek(f, file_size - TRAILER_SIZE, SEEK_SET) != 0) { fclose(f); return false; }
    uint8_t t[TRAILER_SIZE];
    if (fread(t, 1, TRAILER_SIZE, f) != TRAILER_SIZE) { fclose(f); return false; }
    fclose(f);

    if (memcmp(t, JSGLQ_MAGIC, 8) != 0) return false;

    BundleInfo info;
    info.version = read_u32le(t + 8);
    info.offset  = read_u64le(t + 12);
    info.size    = read_u64le(t + 20);
    info.crc     = read_u32le(t + 28);

    /* Consistency: the payload must lie entirely inside the file, before the
       trailer, and be non-empty. A mismatch means a truncated or corrupted fuse,
       which must fail loudly rather than half-load. */
    if (info.size == 0) return false;
    if (info.offset + info.size > (uint64_t)(file_size - TRAILER_SIZE)) return false;
    if (info.version != 1) {
        fprintf(stderr, "jsglq: bundle version %u is newer than this launcher "
                        "supports (1)\n", info.version);
        return false;
    }

    *out = info;
    return true;
}

/*
 * Unpack the embedded payload into `dest_dir`.
 *
 * Payload format (written by scripts/fuse.mjs):
 *   u32  file count
 *   per file: u32 path length, u32 data length, path bytes, data bytes
 *
 * Every path is checked to stay inside dest_dir. A fused binary is as trusted as
 * the person who built it, but a path-traversal check costs nothing and turns a
 * corrupted payload into an error instead of a write to an arbitrary location.
 */
static bool make_parent_dirs(const char *path)
{
    char dir[4096];
    jsglq_dirname(path, dir, sizeof(dir));
    return jsglq_mkdir_p(dir);
}

bool jsglq_bundle_extract(const char *exe_path, const BundleInfo *info,
                          const char *dest_dir)
{
    FILE *in = fopen(exe_path, "rb");
    if (!in) return false;
    if (fseek(in, (long)info->offset, SEEK_SET) != 0) { fclose(in); return false; }

    uint8_t hdr[8];
    if (fread(hdr, 1, 4, in) != 4) { fclose(in); return false; }
    uint32_t count = read_u32le(hdr);
    if (count > 100000) { fclose(in); return false; }

    if (!jsglq_mkdir_p(dest_dir)) { fclose(in); return false; }

    for (uint32_t i = 0; i < count; i++) {
        if (fread(hdr, 1, 8, in) != 8) { fclose(in); return false; }
        uint32_t path_len = read_u32le(hdr);
        uint32_t data_len = read_u32le(hdr + 4);
        if (path_len == 0 || path_len > 1024) { fclose(in); return false; }

        char rel[1025];
        if (fread(rel, 1, path_len, in) != path_len) { fclose(in); return false; }
        rel[path_len] = 0;

        /* Refuse absolute paths and any traversal component. */
        if (rel[0] == '/' || strstr(rel, "..")) { fclose(in); return false; }

        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dest_dir, rel);
        if (!make_parent_dirs(full)) { fclose(in); return false; }

        FILE *out = fopen(full, "wb");
        if (!out) { fclose(in); return false; }
        uint8_t buf[65536];
        uint32_t left = data_len;
        while (left > 0) {
            size_t want = left > sizeof(buf) ? sizeof(buf) : left;
            size_t got = fread(buf, 1, want, in);
            if (got == 0) { fclose(out); fclose(in); return false; }
            if (fwrite(buf, 1, got, out) != got) { fclose(out); fclose(in); return false; }
            left -= (uint32_t)got;
        }
        fclose(out);
    }

    fclose(in);
    return true;
}

/*
 * Resolve the fused game directory, unpacking it on first run.
 *
 * Content-addressed by the payload CRC so re-runs of the same binary reuse the same
 * directory, and a rebuilt binary gets a fresh one rather than mixing old and new
 * files. Games need a real on-disk root: `new URL('x', import.meta.url)`, relative
 * fetches, and asset paths all resolve against it, and an in-memory VFS would mean
 * reimplementing that surface for no benefit.
 */
bool jsglq_bundle_prepare(const char *exe_path, char *out_dir, size_t out_sz)
{
    BundleInfo info;
    if (!jsglq_bundle_find(exe_path, &info)) return false;

    char tmp[4096];
    jsglq_temp_dir(tmp, sizeof(tmp));
    snprintf(out_dir, out_sz, "%s/jsglq-fused-%08x", tmp, info.crc);

    char stamp[4096];
    snprintf(stamp, sizeof(stamp), "%s/.unpacked", out_dir);
    if (jsglq_is_file(stamp)) return true;     /* already unpacked */

    if (!jsglq_bundle_extract(exe_path, &info, out_dir)) {
        fprintf(stderr, "jsglq: fused payload could not be unpacked to %s\n", out_dir);
        return false;
    }
    FILE *f = fopen(stamp, "wb");
    if (f) fclose(f);
    return true;
}
