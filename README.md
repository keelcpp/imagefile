# keelcpp/imagefile

Lightweight C++20 image decoding, encoding, and header inspection library built on [`keelcpp/keel`](https://github.com/keelcpp/keel) and [`keelcpp/compress`](https://github.com/keelcpp/compress) under the `0BSD` license.

## Supported Formats

- **PNG:** 1/2/4/8/16-bit grayscale, grayscale+alpha, RGB, RGBA, indexed (`PLTE`), `tRNS` transparency (color-key and indexed alpha), and Adam7 interlacing (decode, encode, peek).
- **JPEG:** Baseline sequential (single-scan & multi-scan non-interleaved) & progressive (`SOF2`) grayscale and RGB with 4:4:4/4:2:2/4:2:0 subsampling, triangle (`3:1` / `9:3:3:1`) chroma upsampling, custom quality levels, and restart markers (decode, encode, peek).
- **TIFF & BigTIFF:** Classic 32-bit and 64-bit BigTIFF, `U8`/`U16`/`U32`/`I8`/`I16`/`I32`/`F32`/`F64` sample types, `None`/`PackBits`/`LZW`/`Deflate` compression, horizontal differencing (`Predictor == 2`) and Adobe floating-point (`Predictor == 3`) predictors, strips/tiles, chunky/planar layouts, indexed `ColorMap`, and multi-page read/append with automatic Classic-to-BigTIFF upgrade (decode, encode, peek).
- **BMP:** 1/4/8/16/24/32-bit uncompressed, `BI_RLE8`, `BI_RLE4`, and `BI_BITFIELDS` (including 16-bit RGB565 and `BITMAPV4HEADER` 32-bit RGBA) Windows & OS/2 bitmaps (decode, encode, peek).
- **TGA:** 8/16/24/32-bit uncompressed, colormapped, and RLE Truevision TGA (decode, encode, peek).
- **PNM (Netpbm):** ASCII and binary PBM (`P1` including packed bits / `P4`), PGM (`P2`/`P5`), and PPM (`P3`/`P6`) in 8-bit and 16-bit depth (decode, encode, peek).
- **GIF:** `GIF87a`/`GIF89a` multi-frame LZW indexed & interlaced streams with global/local color tables, sub-rectangle placement, Graphic Control Extension transparency, and frame disposal methods (decode, peek).

## Reference Verification

Run `tests/verify_reference.py <path-to-test_imagefile>` to execute bidirectional differential checks against Pillow (`libpng`/`libjpeg-turbo`/`libtiff`), FFmpeg, and MATLAB (`imread`/`imwrite`/`Tiff`) plus an adversarial fuzzing corpus across all 7 formats.

## Design Principles

- **Row-major interleaved pixels:** Canonical native-endian `Image` and `ImageView` buffers with lossless indexed palette preservation and `expand_palette`.
- **Zero STL header bloat & exception-neutral:** Compiles cleanly with `-fno-exceptions -fno-rtti`, returns `keel::Result<T>`, and routes allocations through explicit `keel::Allocator*` and thread-local `keel::Arena` scratch memory.

## CMake Integration

Links as `keel::imagefile` (`find_package(keel_imagefile)` or `FetchContent`).
