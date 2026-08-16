# Third-party components

Everything the launcher binary contains, and what each one requires of you.

**Short version: you can ship a commercial game fused into this binary.** Every
component below permits commercial use and redistribution. The only obligation is
attribution — include this file (or the notices in it) with your game.

| component | license | notes |
|---|---|---|
| [quickjs-ng](https://github.com/quickjs-ng/quickjs) | MIT | the JavaScript engine, statically linked |
| [SDL2](https://www.libsdl.org/) | zlib | window, input, audio device |
| [stb_image, stb_truetype](https://github.com/nothings/stb) | public domain / MIT | image decoding and font rasterization |
| [native-gles](https://github.com/monteslu/native-gles) | ISC | EGL + OpenGL ES 3.0 bindings, compiled from source |
| [webgl-node](https://github.com/monteslu/webgl-node) | ISC | WebGL2 layer, vendored unmodified |
| [webaudio-node](https://github.com/monteslu/webaudio-node) | ISC | Web Audio DSP engine, compiled natively |
| dr_mp3, dr_wav, dr_flac | public domain / MIT-0 | audio decoders (via webaudio-node) |
| stb_vorbis | public domain / MIT | OGG Vorbis decoding (via webaudio-node) |
| DejaVu Sans (subset) | Bitstream Vera + Arev | the embedded fallback font — see below |

## Embedded font

The binary embeds a **subset of DejaVu Sans**, reduced to the codepoints the text
renderer can address (Latin-1 plus a small symbol range).

The Bitstream Vera license explicitly permits embedding, redistribution, and
modification including subsetting. It requires that the font not be sold by itself
and that these notices accompany distribution:

> Copyright (c) 2003 by Bitstream, Inc. All Rights Reserved. Bitstream Vera is a
> trademark of Bitstream, Inc.
>
> Copyright (c) 2006 by Tavmjong Bah. All Rights Reserved.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy of
> the fonts accompanying this license ("Fonts") and associated documentation files
> (the "Font Software"), to reproduce and distribute the Font Software, including
> without limitation the rights to use, copy, merge, publish, distribute, and/or
> sell copies of the Font Software, and to permit persons to whom the Font Software
> is furnished to do so, subject to the conditions in the full license text.

Full text: <https://dejavu-fonts.github.io/License.html>

## ANGLE (macOS and Windows only)

On macOS and Windows the GL layer runs on [ANGLE](https://chromium.googlesource.com/angle/angle)
(BSD-3-Clause), shipped as `libEGL` / `libGLESv2` alongside the executable. Linux
uses the system's own EGL/GLES drivers and ships nothing extra.

## If you are shipping a game

Include this file with your distribution. That satisfies every attribution
requirement above. Nothing here obliges you to open-source your game, disclose your
source, or pay a royalty.
