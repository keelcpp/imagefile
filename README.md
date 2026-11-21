# keelcpp/imagefile

Lightweight C++20 image decoding, encoding, and header inspection library built on [`keelcpp/keel`](https://github.com/keelcpp/keel) and [`keelcpp/compress`](https://github.com/keelcpp/compress) under the `0BSD` license.

## Supported Formats

- **PNG:** 8-bit and 16-bit grayscale, grayscale+alpha, RGB, RGBA, indexed (`PLTE`), and Adam7 interlacing (decode, encode, peek).
- **JPEG:** Baseline sequential grayscale and RGB with custom quality levels and restart markers (decode, encode, peek).
- **TIFF & BigTIFF:** Classic 32-bit and 64-bit BigTIFF, `U8`/`U16`/`U32`/`I8`/`I16`/`I32`/`F32`/`F64` sample types, `None`/`PackBits`/`LZW`/`Deflate` compression, horizontal differencing predictor, strips/tiles, chunky/planar layouts, indexed `ColorMap`, and multi-page read/append (decode, encode, peek).
- **BMP:** 1/4/8/16/24/32-bit uncompressed and `BI_RLE8` Windows & OS/2 bitmaps (decode, encode, peek).
- **TGA:** 8/16/24/32-bit uncompressed, colormapped, and RLE Truevision TGA (decode, encode, peek).
- **PNM (Netpbm):** ASCII and binary PBM (`P1`/`P4`), PGM (`P2`/`P5`), and PPM (`P3`/`P6`) in 8-bit and 16-bit depth (decode, encode, peek).
- **GIF:** `GIF87a`/`GIF89a` LZW indexed and interlaced frames with global/local color tables (decode, peek).

## Design Principles

- **Row-major interleaved pixels:** Canonical native-endian `Image` and `ImageView` buffers with lossless indexed palette preservation and `expand_palette`.
- **Zero STL header bloat & exception-neutral:** Compiles cleanly with `-fno-exceptions -fno-rtti`, returns `keel::Result<T>`, and routes allocations through explicit `keel::Allocator*` and thread-local `keel::Arena` scratch memory.

## CMake Integration

Links as `keel::imagefile` (`find_package(keel_imagefile)` or `FetchContent`).
