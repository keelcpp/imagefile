#include "image_internal.h"

namespace keel::imagefile {

namespace {
namespace tga_impl {

struct TgaHeader {
    u8 id_length = 0;
    u8 color_map_type = 0;
    u8 image_type = 0;
    u16 cmap_start = 0;
    u16 cmap_length = 0;
    u8 cmap_depth = 0;
    u16 width = 0;
    u16 height = 0;
    u8 pixel_depth = 0;
    u8 image_desc = 0;
};

[[nodiscard]] Result<TgaHeader> parse_tga_header(ArrayRef<u8> bytes) noexcept {
    if (bytes.size() < 18) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    const u8* d = bytes.data();

    TgaHeader hdr;
    hdr.id_length = d[0];
    hdr.color_map_type = d[1];
    hdr.image_type = d[2];
    hdr.cmap_start = detail::read_u16_le(d + 3);
    hdr.cmap_length = detail::read_u16_le(d + 5);
    hdr.cmap_depth = d[7];
    hdr.width = detail::read_u16_le(d + 12);
    hdr.height = detail::read_u16_le(d + 14);
    hdr.pixel_depth = d[16];
    hdr.image_desc = d[17];

    if (hdr.color_map_type != 0 && hdr.color_map_type != 1) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (hdr.image_type != 1 && hdr.image_type != 2 && hdr.image_type != 3 && hdr.image_type != 9 &&
        hdr.image_type != 10 && hdr.image_type != 11) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (hdr.width == 0 || hdr.height == 0) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (hdr.pixel_depth != 8 && hdr.pixel_depth != 16 && hdr.pixel_depth != 24 && hdr.pixel_depth != 32) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if ((hdr.image_type == 1 || hdr.image_type == 9) &&
        (hdr.color_map_type != 1 || hdr.cmap_length == 0 ||
         (hdr.cmap_depth != 15 && hdr.cmap_depth != 16 && hdr.cmap_depth != 24 && hdr.cmap_depth != 32))) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    return hdr;
}

[[nodiscard]] constexpr u16 output_channels(const TgaHeader& hdr) noexcept {
    if (hdr.image_type == 3 || hdr.image_type == 11) return 1;
    if (hdr.image_type == 1 || hdr.image_type == 9) return (hdr.cmap_depth == 32) ? 4 : 3;
    if (hdr.pixel_depth == 8) return 1;
    if (hdr.pixel_depth == 32) return 4;
    return 3;
}

} // namespace tga_impl
} // namespace

Result<ImageInfo> peek_tga(ArrayRef<u8> bytes) {
    KEEL_TRY_ASSIGN(const tga_impl::TgaHeader hdr, tga_impl::parse_tga_header(bytes));

    ImageInfo info;
    info.format = ImageFormat::Tga;
    info.width = hdr.width;
    info.height = hdr.height;
    info.channels = tga_impl::output_channels(hdr);
    info.bits_per_sample = 8;
    info.sample_type = SampleType::U8;
    info.has_palette = (hdr.image_type == 1 || hdr.image_type == 9);
    info.page_count = 1;
    return info;
}

Result<Image> decode_tga(ArrayRef<u8> bytes, const ImageDecodeOptions& opts) {
    KEEL_TRY_ASSIGN(const tga_impl::TgaHeader hdr, tga_impl::parse_tga_header(bytes));

    const u16 out_channels = tga_impl::output_channels(hdr);
    KEEL_TRY_ASSIGN(const usize out_bytes,
                    detail::validate_decode_allocation(hdr.width, hdr.height, out_channels, SampleType::U8, opts));

    const usize len = bytes.size();
    const u8* data = bytes.data();
    usize offset = 18u + static_cast<usize>(hdr.id_length);
    if (len < offset) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    Allocator* dst_alloc = opts.allocator ? opts.allocator : default_allocator();
    ArenaTemp scratch = get_scratch(dst_alloc);

    Array<u8> cmap_rgba(scratch.allocator());
    if (hdr.color_map_type == 1 && hdr.cmap_length > 0) {
        const usize entry_bytes = (hdr.cmap_depth == 15) ? 2u : static_cast<usize>(hdr.cmap_depth / 8u);
        const usize cmap_bytes = static_cast<usize>(hdr.cmap_length) * entry_bytes;
        if (len - offset < cmap_bytes) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        cmap_rgba.assign(static_cast<usize>(hdr.cmap_length) * 4u, u8{255});
        for (usize i = 0; i < hdr.cmap_length; ++i) {
            const u8* e = data + offset + i * entry_bytes;
            if (entry_bytes == 2u) {
                const u16 v = detail::read_u16_le(e);
                cmap_rgba[i * 4u + 0u] = static_cast<u8>((((v >> 10) & 0x1Fu) * 255u) / 31u);
                cmap_rgba[i * 4u + 1u] = static_cast<u8>((((v >> 5) & 0x1Fu) * 255u) / 31u);
                cmap_rgba[i * 4u + 2u] = static_cast<u8>(((v & 0x1Fu) * 255u) / 31u);
            } else {
                cmap_rgba[i * 4u + 0u] = e[2];
                cmap_rgba[i * 4u + 1u] = e[1];
                cmap_rgba[i * 4u + 2u] = e[0];
                if (entry_bytes == 4u) {
                    cmap_rgba[i * 4u + 3u] = e[3];
                }
            }
        }
        offset += cmap_bytes;
    }

    const usize w = static_cast<usize>(hdr.width);
    const usize h = static_cast<usize>(hdr.height);
    const usize total_pixels = w * h;
    const usize src_bpp = static_cast<usize>(hdr.pixel_depth / 8u);
    const bool is_rle = (hdr.image_type == 9 || hdr.image_type == 10 || hdr.image_type == 11);
    const bool top_down = (hdr.image_desc & 0x20u) != 0u;
    const bool right_to_left = (hdr.image_desc & 0x10u) != 0u;

    Array<u8> raw_pixels(total_pixels * src_bpp, u8{0}, scratch.allocator());

    if (!is_rle) {
        if (len - offset < total_pixels * src_bpp) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        mem_copy(raw_pixels.data(), data + offset, total_pixels * src_bpp);
    } else {
        usize pixel_idx = 0;
        usize p = offset;
        u8* rdst = raw_pixels.data();
        while (pixel_idx < total_pixels && p < len) {
            const u8 pkt = data[p++];
            const usize count = static_cast<usize>(pkt & 0x7Fu) + 1u;
            if ((pkt & 0x80u) != 0u) {
                if (len - p < src_bpp) [[unlikely]] {
                    return ErrorCode::EndOfFile;
                }
                const u8* px = data + p;
                p += src_bpp;
                const usize run = keel::min(count, total_pixels - pixel_idx);
                if (count > total_pixels - pixel_idx) {
                    // Clamp safely at image boundary
                }
                for (usize k = 0; k < run; ++k) {
                    mem_copy(rdst + (pixel_idx + k) * src_bpp, px, src_bpp);
                }
                pixel_idx += run;
            } else {
                const usize run = keel::min(count, total_pixels - pixel_idx);
                if (len - p < run * src_bpp) [[unlikely]] {
                    return ErrorCode::EndOfFile;
                }
                mem_copy(rdst + pixel_idx * src_bpp, data + p, run * src_bpp);
                p += count * src_bpp > (len - p) ? (len - p) : (count * src_bpp);
                pixel_idx += run;
            }
        }
        if (pixel_idx < total_pixels) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
    }

    Image img;
    img.width = hdr.width;
    img.height = hdr.height;
    img.channels = out_channels;
    img.sample_type = SampleType::U8;
    img.palette_bits = 0;
    img.pixels = Array<u8>(out_bytes, u8{0}, dst_alloc);
    img.palette = Array<u16>(dst_alloc);

    u8* dst = img.pixels.data();
    const bool is_indexed = (hdr.image_type == 1 || hdr.image_type == 9);

    for (usize r = 0; r < h; ++r) {
        const usize src_y = top_down ? r : (h - 1u - r);
        const u8* srow = raw_pixels.data() + src_y * w * src_bpp;
        u8* drow = dst + r * w * static_cast<usize>(out_channels);

        for (usize c = 0; c < w; ++c) {
            const usize src_x = right_to_left ? (w - 1u - c) : c;
            const u8* spx = srow + src_x * src_bpp;
            u8* dpx = drow + c * static_cast<usize>(out_channels);

            if (is_indexed) {
                const usize raw_idx = (src_bpp == 1u) ? static_cast<usize>(spx[0]) : detail::read_u16_le(spx);
                if (raw_idx >= hdr.cmap_start && (raw_idx - hdr.cmap_start) < hdr.cmap_length) {
                    const usize ci = raw_idx - hdr.cmap_start;
                    dpx[0] = cmap_rgba[ci * 4u + 0u];
                    dpx[1] = cmap_rgba[ci * 4u + 1u];
                    dpx[2] = cmap_rgba[ci * 4u + 2u];
                    if (out_channels == 4) dpx[3] = cmap_rgba[ci * 4u + 3u];
                }
            } else if (src_bpp == 1u) {
                dpx[0] = spx[0];
            } else if (src_bpp == 2u) {
                const u16 v = detail::read_u16_le(spx);
                dpx[0] = static_cast<u8>((((v >> 10) & 0x1Fu) * 255u) / 31u);
                dpx[1] = static_cast<u8>((((v >> 5) & 0x1Fu) * 255u) / 31u);
                dpx[2] = static_cast<u8>(((v & 0x1Fu) * 255u) / 31u);
            } else if (src_bpp == 3u) {
                dpx[0] = spx[2];
                dpx[1] = spx[1];
                dpx[2] = spx[0];
            } else {
                dpx[0] = spx[2];
                dpx[1] = spx[1];
                dpx[2] = spx[0];
                dpx[3] = spx[3];
            }
        }
    }

    return img;
}

Result<Array<u8>> encode_tga(const ImageView& view, Allocator* alloc) {
    KEEL_TRY_ASSIGN(const detail::ValidatedView vv, detail::validate_image_view(view));
    if (view.sample_type != SampleType::U8) [[unlikely]] {
        return ErrorCode::NotSupported;
    }
    if (view.channels != 1 && view.channels != 3 && view.channels != 4) [[unlikely]] {
        return ErrorCode::NotSupported;
    }
    if (view.width > 65535u || view.height > 65535u) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }

    const usize w = static_cast<usize>(view.width);
    const usize h = static_cast<usize>(view.height);
    const usize pixel_bytes = w * h * static_cast<usize>(view.channels);

    Allocator* dst_alloc = alloc ? alloc : default_allocator();
    Array<u8> out(dst_alloc);
    out.reserve(18u + pixel_bytes);

    const u8 image_type = (view.channels == 1) ? 3u : 2u;
    const u8 pixel_depth = static_cast<u8>(view.channels * 8u);
    const u8 image_desc = (view.channels == 4) ? 0x28u : 0x20u; // Top-left origin (+ 8 alpha bits when RGBA)

    out.push_back(0u);
    out.push_back(0u);
    out.push_back(image_type);
    detail::push_u16_le(out, 0u);
    detail::push_u16_le(out, 0u);
    out.push_back(0u);
    detail::push_u16_le(out, 0u);
    detail::push_u16_le(out, 0u);
    detail::push_u16_le(out, static_cast<u16>(view.width));
    detail::push_u16_le(out, static_cast<u16>(view.height));
    out.push_back(pixel_depth);
    out.push_back(image_desc);

    out.resize(18u + pixel_bytes);
    u8* dst = out.data() + 18u;

    for (usize r = 0; r < h; ++r) {
        const u8* srow = view.pixels.data() + r * vv.stride_bytes;
        u8* drow = dst + r * w * static_cast<usize>(view.channels);
        if (view.channels == 1) {
            mem_copy(drow, srow, w);
        } else if (view.channels == 3) {
            for (usize c = 0; c < w; ++c) {
                drow[c * 3u + 0u] = srow[c * 3u + 2u];
                drow[c * 3u + 1u] = srow[c * 3u + 1u];
                drow[c * 3u + 2u] = srow[c * 3u + 0u];
            }
        } else {
            for (usize c = 0; c < w; ++c) {
                drow[c * 4u + 0u] = srow[c * 4u + 2u];
                drow[c * 4u + 1u] = srow[c * 4u + 1u];
                drow[c * 4u + 2u] = srow[c * 4u + 0u];
                drow[c * 4u + 3u] = srow[c * 4u + 3u];
            }
        }
    }

    return out;
}

} // namespace keel::imagefile
