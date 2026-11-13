# Speex 1.2.1 (decoder)

The decoder parts of libspeex from the official release
[speex-1.2.1.tar.gz](https://downloads.xiph.org/releases/speex/speex-1.2.1.tar.gz)
(SHA-256 4b44d4f2b38a370a2d98a78329fefc56a0cf93d1c1be70029217baae6628feea, as xiph's SHA256SUMS.txt lists it),
unchanged: `libspeex/*.c` and `*.h` except the tests and the Vorbis psychoacoustic and FFT files only the encoder's
VORBIS_PSYCHO option uses, and `include/speex/*.h`. `speex_config_types.h` is written by hand in place of the one
configure makes. Built with FLOATING_POINT. BSD licensed (COPYING).

The game's voices (and many pack sound effects) are Speex clips ("SPX1"), decoded by src/audio/spx.cpp.
