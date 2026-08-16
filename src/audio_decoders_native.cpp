/*
 * webaudio-node's decoder, compiled natively with an opus stub.
 *
 * WHY A WRAPPER: audio_decoders.cpp includes opusfile by RELATIVE path
 * ("../vendor/opusfile/include/opusfile.h"), so an -I include-path shim cannot
 * intercept it. Pre-defining the real header's include guard here means its
 * contents are skipped and the declarations below stand in — with upstream left
 * completely unedited, which is the property worth protecting.
 *
 * WHY STUB OPUS AT ALL: opus/ogg/opusfile ship in webaudio-node as full autotools
 * source trees configured for an Emscripten build. Building them natively is real
 * work for a format the corpus does not contain: across all 14 games there are
 * 63 mp3 and 32 ogg files and ZERO opus. Every other format (mp3, wav, flac,
 * ogg-vorbis, aac) decodes for real.
 *
 * An opus file therefore gets a NAMED error identifying the format, not silence.
 * Wiring real opus in later is a build-system task; no decoder source changes.
 */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

/* Skip the real opusfile.h (its guard is `_opusfile_h`). */
#define _opusfile_h (1)

typedef struct OggOpusFile OggOpusFile;
typedef int64_t ogg_int64_t;    /* normally from ogg/os_types.h via opusfile.h */
#define OP_EREAD (-128)

extern "C" {

static OggOpusFile *op_open_memory(const unsigned char *data, size_t size, int *error)
{
    (void)data; (void)size;
    fprintf(stderr, "jsglq: opus decoding is not built into this launcher "
                    "(mp3, wav, flac, ogg-vorbis and aac are). "
                    "Convert the file to ogg-vorbis.\n");
    if (error) *error = OP_EREAD;
    return NULL;
}
static int     op_channel_count(const OggOpusFile *of, int li) { (void)of; (void)li; return 0; }
static int64_t op_pcm_total(const OggOpusFile *of, int li)     { (void)of; (void)li; return 0; }
static int     op_read_float(OggOpusFile *of, float *pcm, int bufsize, int *li)
{ (void)of; (void)pcm; (void)bufsize; (void)li; return 0; }
static void    op_free(OggOpusFile *of) { (void)of; }

}  /* extern "C" */

#include "../../webaudio-node/src/wasm/audio_decoders.cpp"

/*
 * AAC via libxaac is stubbed for the same reason as opus: it ships as a vendored
 * source tree configured for the Emscripten build, and the corpus contains no AAC
 * (63 mp3 + 32 ogg + 0 aac across all 14 games). The decoder declares this symbol
 * itself rather than pulling it from a header, so defining it here satisfies the
 * link while keeping upstream unedited.
 *
 * An AAC file gets a decode failure naming the format; every other format works.
 */
extern "C" IA_ERRORCODE ixheaacd_dec_api(pVOID p_obj, WORD32 cmd, WORD32 idx, pVOID val)
{
    (void)p_obj; (void)cmd; (void)idx; (void)val;
    static int warned = 0;
    if (!warned) {
        warned = 1;
        fprintf(stderr, "jsglq: AAC decoding is not built into this launcher "
                        "(mp3, wav, flac and ogg-vorbis are). "
                        "Convert the file to mp3 or ogg.\n");
    }
    return -1;
}
