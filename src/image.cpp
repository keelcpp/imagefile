#include "image_internal.h"

#include "keel/fs.h"

namespace keel::imagefile {

namespace {
namespace image_impl {

[[nodiscard]] constexpr bool is_pnm_delim(u8 c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == '#';
}

[[nodiscard]] ImageFormat format_from_extension(StringRef ext) noexcept {
    if (ext.starts_with('.')) {
        ext.remove_prefix(1);
    }
    if (ext.equals_ignore_ascii_case("png")) return ImageFormat::Png;
    if (ext.equals_ignore_ascii_case("jpg") || ext.equals_ignore_ascii_case("jpeg")) return ImageFormat::Jpeg;
    if (ext.equals_ignore_ascii_case("tif") || ext.equals_ignore_ascii_case("tiff")) return ImageFormat::Tiff;
    if (ext.equals_ignore_ascii_case("bmp") || ext.equals_ignore_ascii_case("dib")) return ImageFormat::Bmp;
    if (ext.equals_ignore_ascii_case("tga")) return ImageFormat::Tga;
    if (ext.equals_ignore_ascii_case("pnm") || ext.equals_ignore_ascii_case("pbm") ||
        ext.equals_ignore_ascii_case("pgm") || ext.equals_ignore_ascii_case("ppm")) {
        return ImageFormat::Pnm;
    }
    if (ext.equals_ignore_ascii_case("gif")) return ImageFormat::Gif;
    return ImageFormat::Unknown;
}

} // namespace image_impl
} // namespace

ImageFormat sniff_image_format(ArrayRef<u8> bytes) noexcept {
    const usize n = bytes.size();
    if (n < 2) return ImageFormat::Unknown;
    const u8* hdr = bytes.data();

    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (n >= 8 && hdr[0] == 0x89u && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G' && hdr[4] == 0x0Du &&
        hdr[5] == 0x0Au && hdr[6] == 0x1Au && hdr[7] == 0x0Au) {
        return ImageFormat::Png;
    }

    // TIFF / BigTIFF
    if (is_tiff_bytes(bytes)) {
        return ImageFormat::Tiff;
    }

    // JPEG: FF D8 FF
    if (n >= 3 && hdr[0] == 0xFFu && hdr[1] == 0xD8u && hdr[2] == 0xFFu) {
        return ImageFormat::Jpeg;
    }

    // BMP: 'BM'
    if (hdr[0] == 'B' && hdr[1] == 'M') {
        return ImageFormat::Bmp;
    }

    // GIF: 'GIF87a' or 'GIF89a'
    if (n >= 6 && hdr[0] == 'G' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == '8' &&
        (hdr[4] == '7' || hdr[4] == '9') && hdr[5] == 'a') {
        return ImageFormat::Gif;
    }

    // PNM: 'P1' .. 'P6' followed by whitespace or comment
    if (n >= 3 && hdr[0] == 'P' && hdr[1] >= '1' && hdr[1] <= '6' && image_impl::is_pnm_delim(hdr[2])) {
        return ImageFormat::Pnm;
    }

    // TGA: 18-byte header with valid colorMapType, imageType, dimensions, and pixelDepth
    if (n >= 18) {
        const u8 cmap_type = hdr[1];
        const u8 img_type = hdr[2];
        const u16 w = detail::read_u16_le(hdr + 12);
        const u16 h = detail::read_u16_le(hdr + 14);
        const u8 depth = hdr[16];
        const bool valid_type =
            (img_type == 1 || img_type == 2 || img_type == 3 || img_type == 9 || img_type == 10 || img_type == 11);
        const bool valid_depth = (depth == 8 || depth == 16 || depth == 24 || depth == 32);
        if ((cmap_type == 0 || cmap_type == 1) && valid_type && valid_depth && w > 0 && h > 0) {
            return ImageFormat::Tga;
        }
    }

    return ImageFormat::Unknown;
}

Result<Image> expand_palette(const Image& indexed, Allocator* alloc) {
    if (indexed.width == 0 || indexed.height == 0 || indexed.channels != 1 || !indexed.has_palette()) [[unlikely]] {
        return ErrorCode::InvalidArgument;
    }
    if (indexed.palette.size() % 3u != 0u || indexed.palette.empty()) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (indexed.sample_type != SampleType::U8 && indexed.sample_type != SampleType::U16) [[unlikely]] {
        return ErrorCode::NotSupported;
    }
    if (indexed.palette_bits != 8 && indexed.palette_bits != 16) [[unlikely]] {
        return ErrorCode::NotSupported;
    }

    const usize num_pixels = as<usize>(indexed.width) * as<usize>(indexed.height);
    const usize idx_bps = sample_byte_size(indexed.sample_type);
    if (indexed.pixels.size() < num_pixels * idx_bps) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    Allocator* dst_alloc = alloc ? alloc : indexed.pixels.allocator();
    const SampleType out_type = (indexed.palette_bits == 16) ? SampleType::U16 : SampleType::U8;
    const usize out_bps = sample_byte_size(out_type);
    if (num_pixels > Limits<usize>::kMax / (3u * out_bps)) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }

    Image out;
    out.width = indexed.width;
    out.height = indexed.height;
    out.channels = 3;
    out.sample_type = out_type;
    out.palette_bits = 0;
    out.pixels = Array<u8>(num_pixels * 3u * out_bps, u8{0}, dst_alloc);
    out.palette = Array<u16>(dst_alloc);

    const usize num_entries = indexed.palette.size() / 3u;
    const u16* pal = indexed.palette.data();
    const u8* src = indexed.pixels.data();
    u8* dst = out.pixels.data();

    for (usize i = 0; i < num_pixels; ++i) {
        const usize idx =
            (indexed.sample_type == SampleType::U8) ? as<usize>(src[i]) : detail::read_u16_native(src + i * 2u);
        u16 r = 0;
        u16 g = 0;
        u16 b = 0;
        if (idx < num_entries) {
            r = pal[idx * 3u + 0u];
            g = pal[idx * 3u + 1u];
            b = pal[idx * 3u + 2u];
        }
        if (out_type == SampleType::U8) {
            dst[i * 3u + 0u] = as<u8>(r & 0xFFu);
            dst[i * 3u + 1u] = as<u8>(g & 0xFFu);
            dst[i * 3u + 2u] = as<u8>(b & 0xFFu);
        } else {
            detail::write_u16_native(dst + (i * 3u + 0u) * 2u, r);
            detail::write_u16_native(dst + (i * 3u + 1u) * 2u, g);
            detail::write_u16_native(dst + (i * 3u + 2u) * 2u, b);
        }
    }

    return out;
}

Result<ImageInfo> peek_image(ArrayRef<u8> bytes, ImageFormat hint) {
    if (bytes.empty()) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    ImageFormat fmt = sniff_image_format(bytes);
    if (fmt == ImageFormat::Unknown) {
        fmt = hint;
    }

    switch (fmt) {
        case ImageFormat::Png:
            return peek_png(bytes);
        case ImageFormat::Jpeg:
            return peek_jpeg(bytes);
        case ImageFormat::Tiff:
            return peek_tiff(bytes);
        case ImageFormat::Bmp:
            return peek_bmp(bytes);
        case ImageFormat::Tga:
            return peek_tga(bytes);
        case ImageFormat::Pnm:
            return peek_pnm(bytes);
        case ImageFormat::Gif:
            return peek_gif(bytes);
        case ImageFormat::Unknown:
            break;
    }

    if (auto r = peek_png(bytes); r.is_ok()) return r;
    if (auto r = peek_bmp(bytes); r.is_ok()) return r;
    if (auto r = peek_jpeg(bytes); r.is_ok()) return r;
    if (auto r = peek_tga(bytes); r.is_ok()) return r;

    return (bytes.size() < 4) ? ErrorCode::EndOfFile : ErrorCode::InvalidData;
}

Result<Image> decode_image(ArrayRef<u8> bytes, const ImageDecodeOptions& opts) {
    if (bytes.empty()) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    const ImageFormat fmt = sniff_image_format(bytes);
    switch (fmt) {
        case ImageFormat::Png:
            return decode_png(bytes, opts);
        case ImageFormat::Tiff:
            return decode_tiff(bytes, opts);
        case ImageFormat::Jpeg:
            return decode_jpeg(bytes, opts);
        case ImageFormat::Bmp:
            return decode_bmp(bytes, opts);
        case ImageFormat::Gif:
            return decode_gif(bytes, opts);
        case ImageFormat::Pnm:
            return decode_pnm(bytes, opts);
        case ImageFormat::Tga:
            return decode_tga(bytes, opts);
        case ImageFormat::Unknown:
            break;
    }

    // Fallback probe matching PNG -> BMP -> JPEG -> TGA
    if (auto r = decode_png(bytes, opts); r.is_ok() || r.error().code == ErrorCode::OutOfRange) return r;
    if (auto r = decode_bmp(bytes, opts); r.is_ok() || r.error().code == ErrorCode::OutOfRange) return r;
    if (auto r = decode_jpeg(bytes, opts); r.is_ok() || r.error().code == ErrorCode::OutOfRange) return r;
    if (auto r = decode_tga(bytes, opts); r.is_ok() || r.error().code == ErrorCode::OutOfRange) return r;

    return (bytes.size() < 4) ? ErrorCode::EndOfFile : ErrorCode::InvalidData;
}

Result<ImageInfo> peek_image_file(StringRef path) {
    ArenaTemp scratch = get_scratch();
    KEEL_TRY_ASSIGN(Array<u8> bytes, fs_read_file(path, scratch.allocator()));
    const ImageFormat hint = image_impl::format_from_extension(path_extension(path));
    return peek_image(bytes, hint);
}

Result<Image> read_image(StringRef path, const ImageDecodeOptions& opts) {
    ArenaTemp scratch = get_scratch(opts.allocator);
    KEEL_TRY_ASSIGN(Array<u8> bytes, fs_read_file(path, scratch.allocator()));
    return decode_image(bytes, opts);
}

Result<Array<u8>> encode_image(const ImageView& view, ImageFormat fmt, Allocator* alloc) {
    switch (fmt) {
        case ImageFormat::Png:
            return encode_png(view, PngEncodeOptions{.compression_level = 6, .allocator = alloc});
        case ImageFormat::Jpeg:
            return encode_jpeg(view, JpegEncodeOptions{.quality = 90, .allocator = alloc});
        case ImageFormat::Tiff:
            return encode_tiff(view, TiffEncodeOptions{.compression = TiffCompression::None, .allocator = alloc});
        case ImageFormat::Bmp:
            return encode_bmp(view, alloc);
        case ImageFormat::Tga:
            return encode_tga(view, alloc);
        case ImageFormat::Pnm:
            return encode_pnm(view, PnmEncodeOptions{.subformat = PnmSubformat::Auto, .allocator = alloc});
        case ImageFormat::Gif:
            return ErrorCode::NotSupported;
        case ImageFormat::Unknown:
            return ErrorCode::InvalidArgument;
    }
    return ErrorCode::InvalidArgument;
}

Result<void> write_image(StringRef path, const ImageView& view) {
    StringRef ext = path_extension(path);
    if (ext.starts_with('.')) {
        ext.remove_prefix(1);
    }
    if (ext.empty()) [[unlikely]] {
        return ErrorCode::InvalidArgument;
    }

    ArenaTemp scratch = get_scratch();
    Array<u8> encoded(scratch.allocator());

    if (ext.equals_ignore_ascii_case("pbm")) {
        KEEL_TRY_ASSIGN(
            encoded, encode_pnm(view, PnmEncodeOptions{.subformat = PnmSubformat::Pbm, .allocator = scratch.allocator()}));
    } else if (ext.equals_ignore_ascii_case("pgm")) {
        KEEL_TRY_ASSIGN(
            encoded, encode_pnm(view, PnmEncodeOptions{.subformat = PnmSubformat::Pgm, .allocator = scratch.allocator()}));
    } else if (ext.equals_ignore_ascii_case("ppm")) {
        KEEL_TRY_ASSIGN(
            encoded, encode_pnm(view, PnmEncodeOptions{.subformat = PnmSubformat::Ppm, .allocator = scratch.allocator()}));
    } else {
        const ImageFormat fmt = image_impl::format_from_extension(ext);
        if (fmt == ImageFormat::Unknown || fmt == ImageFormat::Gif) [[unlikely]] {
            return ErrorCode::NotSupported;
        }
        KEEL_TRY_ASSIGN(encoded, encode_image(view, fmt, scratch.allocator()));
    }

    KEEL_TRY(fs_write_file(path, encoded));
    return {};
}

} // namespace keel::imagefile
