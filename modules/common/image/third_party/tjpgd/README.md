# TJpgDec

Source: ChaN, [TJpgDec R0.03](https://elm-chan.org/fsw/tjpgd/arc/tjpgd3.zip).
The upstream license is retained in `tjpgd.c`.

This module builds the decoder independently of LVGL with grayscale output,
1/2, 1/4 and 1/8 scaling, a 2048-byte input buffer and fast Huffman tables.
Signed IDCT output is saturated before grayscale conversion, including the
1/8 DC output path. The grayscale IDCT saturation follows upstream patch1.

The SDK decoder and hardware JPEG driver are not replaced by these sources.

The shared image adapter and decoder both include the private `epd_tjpgd.h`
and its `epd_tjpgdcnf.h` configuration. These names keep the `JDEC` layout
independent of SDK `tjpgd.h` search paths. Books and gallery use this same
decoder configuration.
