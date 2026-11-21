#pragma once

#include "keel/array.h"
#include "keel/result.h"

namespace keel::imagefile {

enum class ImageFormat : u8 {
    Unknown = 0,
    Png,
    Jpeg,
    Tiff,
    Bmp,
    Tga,
    Pnm,
    Gif,
};

enum class SampleType : u8 {
    U8 = 0,
    U16,
    U32,
    I8,
    I16,
    I32,
    F32,
    F64,
};

[[nodiscard]] constexpr usize sample_byte_size(SampleType t) noexcept {
    switch (t) {
        case SampleType::U8:
        case SampleType::I8:
            return 1;
        case SampleType::U16:
        case SampleType::I16:
            return 2;
        case SampleType::U32:
        case SampleType::I32:
        case SampleType::F32:
            return 4;
        case SampleType::F64:
            return 8;
    }
    return 1;
}

[[nodiscard]] constexpr u16 sample_bit_depth(SampleType t) noexcept {
    return as<u16>(sample_byte_size(t) * 8u);
}

struct ImageInfo {
    ImageFormat format = ImageFormat::Unknown;
    u32 width = 0;
    u32 height = 0;
    u16 channels = 0;
    u16 bits_per_sample = 0;
    SampleType sample_type = SampleType::U8;
    bool has_palette = false;
    u32 page_count = 1;
};

struct ImageDecodeOptions {
    u32 max_width = 65535;
    u32 max_height = 65535;
    usize max_pixels = 256ULL * 1024 * 1024;
    usize max_bytes = 512ULL * 1024 * 1024;
    u32 page = 0;               // 0-based IFD page index for multi-page TIFF
    bool expand_indexed = false; // If true, indexed PNG/TIFF/GIF images expand to 3-channel RGB
    Allocator* allocator = nullptr;
};

struct ImageView {
    u32 width = 0;
    u32 height = 0;
    u16 channels = 0;
    SampleType sample_type = SampleType::U8;
    usize row_stride_bytes = 0; // 0 means tightly packed: width * channels * sample_byte_size(sample_type)
    ArrayRef<u8> pixels{};

    [[nodiscard]] constexpr usize effective_row_stride() const noexcept {
        return (row_stride_bytes != 0)
                   ? row_stride_bytes
                   : (as<usize>(width) * as<usize>(channels) * sample_byte_size(sample_type));
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        return width == 0 || height == 0 || channels == 0 || pixels.empty();
    }
};

// Owning row-major interleaved image buffer.
// Pixel (r, c) channel `ch` resides at sample index `(r * width + c) * channels + ch` in native endianness.
// For indexed PNG (`colorType == 3`), TIFF (`Photometric == 3`), and GIF images:
//   - `channels == 1` and `pixels` holds unpacked index values (`SampleType::U8` or `SampleType::U16`).
//   - `palette` holds `3 * N` interleaved RGB entries `[R0, G0, B0, R1, G1, B1, ...]`.
//   - `palette_bits` is `8` for `[0..255]` entries (PNG/GIF) or `16` for `[0..65535]` entries (TIFF).
struct Image {
    u32 width = 0;
    u32 height = 0;
    u16 channels = 0;
    SampleType sample_type = SampleType::U8;
    u8 palette_bits = 0;
    Array<u8> pixels;
    Array<u16> palette;

    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0 || pixels.empty(); }
    [[nodiscard]] bool has_palette() const noexcept { return palette_bits != 0 && !palette.empty(); }
    [[nodiscard]] usize palette_entry_count() const noexcept { return palette.size() / 3u; }
    [[nodiscard]] usize row_stride_bytes() const noexcept {
        return as<usize>(width) * as<usize>(channels) * sample_byte_size(sample_type);
    }

    [[nodiscard]] ImageView view() const noexcept KEEL_LIFETIMEBOUND {
        return ImageView{
            .width = width,
            .height = height,
            .channels = channels,
            .sample_type = sample_type,
            .row_stride_bytes = row_stride_bytes(),
            .pixels = pixels.as_ref(),
        };
    }

    [[nodiscard]] operator ImageView() const noexcept KEEL_LIFETIMEBOUND { return view(); }

    [[nodiscard]] Image clone(Allocator* alloc = nullptr) const {
        Image copy;
        copy.width = width;
        copy.height = height;
        copy.channels = channels;
        copy.sample_type = sample_type;
        copy.palette_bits = palette_bits;
        copy.pixels = pixels.clone(alloc);
        copy.palette = palette.clone(alloc);
        return copy;
    }
};

// Expands an indexed 1-channel `Image` with a populated `palette` into a 3-channel RGB `Image`
// (`SampleType::U8` when `palette_bits == 8`, `SampleType::U16` when `palette_bits == 16`).
[[nodiscard]] Result<Image> expand_palette(const Image& indexed, Allocator* alloc = nullptr);

// Format sniffing & header inspection without decoding pixel payloads.
[[nodiscard]] bool is_tiff_bytes(ArrayRef<u8> bytes) noexcept;
[[nodiscard]] ImageFormat sniff_image_format(ArrayRef<u8> bytes) noexcept;

[[nodiscard]] Result<ImageInfo> peek_image(ArrayRef<u8> bytes, ImageFormat hint = ImageFormat::Unknown);
[[nodiscard]] Result<ImageInfo> peek_png(ArrayRef<u8> bytes);
[[nodiscard]] Result<ImageInfo> peek_jpeg(ArrayRef<u8> bytes);
[[nodiscard]] Result<ImageInfo> peek_tiff(ArrayRef<u8> bytes);
[[nodiscard]] Result<ImageInfo> peek_bmp(ArrayRef<u8> bytes);
[[nodiscard]] Result<ImageInfo> peek_tga(ArrayRef<u8> bytes);
[[nodiscard]] Result<ImageInfo> peek_pnm(ArrayRef<u8> bytes);
[[nodiscard]] Result<ImageInfo> peek_gif(ArrayRef<u8> bytes);
[[nodiscard]] Result<u32> tiff_page_count(ArrayRef<u8> bytes);

// In-memory decoding.
[[nodiscard]] Result<Image> decode_image(ArrayRef<u8> bytes, const ImageDecodeOptions& opts = {});
[[nodiscard]] Result<Image> decode_png(ArrayRef<u8> bytes, const ImageDecodeOptions& opts = {});
[[nodiscard]] Result<Image> decode_jpeg(ArrayRef<u8> bytes, const ImageDecodeOptions& opts = {});
[[nodiscard]] Result<Image> decode_tiff(ArrayRef<u8> bytes, const ImageDecodeOptions& opts = {});
[[nodiscard]] Result<Image> decode_bmp(ArrayRef<u8> bytes, const ImageDecodeOptions& opts = {});
[[nodiscard]] Result<Image> decode_tga(ArrayRef<u8> bytes, const ImageDecodeOptions& opts = {});
[[nodiscard]] Result<Image> decode_pnm(ArrayRef<u8> bytes, const ImageDecodeOptions& opts = {});
[[nodiscard]] Result<Image> decode_gif(ArrayRef<u8> bytes, const ImageDecodeOptions& opts = {});

// File-based inspection & decoding.
[[nodiscard]] Result<ImageInfo> peek_image_file(StringRef path);
[[nodiscard]] Result<Image> read_image(StringRef path, const ImageDecodeOptions& opts = {});

// Encoding options & format encoders.
enum class PnmSubformat : u8 {
    Auto = 0,
    Pbm,
    Pgm,
    Ppm,
};

enum class TiffCompression : u8 {
    None = 0,
    PackBits,
    Lzw,
    Deflate,
};

struct PngEncodeOptions {
    i32 compression_level = 6;
    Allocator* allocator = nullptr;
    ArrayRef<u16> palette{}; // Optional RGB palette [R0, G0, B0, ...] (0..255) for 1-channel U8 indexed PNG
};

struct JpegEncodeOptions {
    i32 quality = 90;
    Allocator* allocator = nullptr;
};

struct TiffEncodeOptions {
    TiffCompression compression = TiffCompression::None;
    Allocator* allocator = nullptr;
    bool bigtiff = false; // Force 64-bit BigTIFF container even when < 4 GiB
};

struct PnmEncodeOptions {
    PnmSubformat subformat = PnmSubformat::Auto;
    Allocator* allocator = nullptr;
};

[[nodiscard]] Result<Array<u8>> encode_png(const ImageView& view, const PngEncodeOptions& opts = {});
[[nodiscard]] Result<Array<u8>> encode_jpeg(const ImageView& view, const JpegEncodeOptions& opts = {});
[[nodiscard]] Result<Array<u8>> encode_bmp(const ImageView& view, Allocator* alloc = nullptr);
[[nodiscard]] Result<Array<u8>> encode_tga(const ImageView& view, Allocator* alloc = nullptr);
[[nodiscard]] Result<Array<u8>> encode_pnm(const ImageView& view, const PnmEncodeOptions& opts = {});
[[nodiscard]] Result<Array<u8>> encode_tiff(
    const ImageView& view, const TiffEncodeOptions& opts = {}, ArrayRef<u8> append_to_existing = {});

[[nodiscard]] Result<Array<u8>> encode_image(const ImageView& view, ImageFormat fmt, Allocator* alloc = nullptr);
[[nodiscard]] Result<void> write_image(StringRef path, const ImageView& view);

} // namespace keel::imagefile
