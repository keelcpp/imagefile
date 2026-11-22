#include "image_internal.h"

namespace keel::imagefile {

namespace {
namespace bmp_impl {

struct BmpHeader {
    u32 file_size = 0;
    u32 off_bits = 0;
    u32 header_size = 0;
    u32 width = 0;
    u32 height = 0;
    u16 bit_count = 0;
    u32 compression = 0;
    u32 num_colors = 0;
    bool top_down = false;
};

[[nodiscard]] Result<BmpHeader> parse_bmp_header(ArrayRef<u8> bytes) noexcept {
    const usize len = bytes.size();
    const u8* data = bytes.data();
    if (len < 2) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    if (data[0] != 'B' || data[1] != 'M') [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (len < 14 + 12) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    BmpHeader hdr;
    hdr.file_size = detail::read_u32_le(data + 2);
    hdr.off_bits = detail::read_u32_le(data + 10);
    hdr.header_size = detail::read_u32_le(data + 14);

    if (hdr.header_size < 12u) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (len < 14u + as<usize>(hdr.header_size)) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    if (hdr.header_size == 12u) {
        hdr.width = detail::read_u16_le(data + 18);
        hdr.height = detail::read_u16_le(data + 20);
        const u16 planes = detail::read_u16_le(data + 22);
        hdr.bit_count = detail::read_u16_le(data + 24);
        if (planes != 1u) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
    } else {
        if (hdr.header_size < 40u) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        const i32 raw_w = as<i32>(detail::read_u32_le(data + 18));
        const i32 raw_h = as<i32>(detail::read_u32_le(data + 22));
        const u16 planes = detail::read_u16_le(data + 26);
        if (raw_w <= 0 || raw_h == 0 || raw_h == as<i32>(0x80000000u) || planes != 1u) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        hdr.width = as<u32>(raw_w);
        if (raw_h < 0) {
            hdr.top_down = true;
            hdr.height = as<u32>(-raw_h);
        } else {
            hdr.height = as<u32>(raw_h);
        }
        hdr.bit_count = detail::read_u16_le(data + 28);
        hdr.compression = detail::read_u32_le(data + 30);
        hdr.num_colors = detail::read_u32_le(data + 46);
    }

    if (hdr.width == 0 || hdr.height == 0) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (hdr.bit_count != 1 && hdr.bit_count != 4 && hdr.bit_count != 8 && hdr.bit_count != 16 && hdr.bit_count != 24 &&
        hdr.bit_count != 32) [[unlikely]] {
        return ErrorCode::NotSupported;
    }
    if (hdr.compression != 0 && hdr.compression != 1 && hdr.compression != 2 && hdr.compression != 3) [[unlikely]] {
        return ErrorCode::NotSupported;
    }

    return hdr;
}

[[nodiscard]] bool is_gray_ramp_palette(ArrayRef<u8> bytes, const BmpHeader& hdr) noexcept {
    if (hdr.bit_count != 8u) return false;
    usize pal_size = (hdr.num_colors != 0) ? as<usize>(hdr.num_colors) : 256u;
    if (pal_size != 256u) return false;
    const usize pal_off = 14u + as<usize>(hdr.header_size);
    const usize entry_bytes = (hdr.header_size == 12u) ? 3u : 4u;
    if (bytes.size() < pal_off || bytes.size() - pal_off < pal_size * entry_bytes) return false;
    const u8* p = bytes.data() + pal_off;
    for (usize i = 0; i < 256u; ++i) {
        const u8 b = p[i * entry_bytes + 0u];
        const u8 g = p[i * entry_bytes + 1u];
        const u8 r = p[i * entry_bytes + 2u];
        if (r != i || g != i || b != i) return false;
    }
    return true;
}

} // namespace bmp_impl
} // namespace

Result<ImageInfo> peek_bmp(ArrayRef<u8> bytes) {
    KEEL_TRY_ASSIGN(const bmp_impl::BmpHeader hdr, bmp_impl::parse_bmp_header(bytes));

    ImageInfo info;
    info.format = ImageFormat::Bmp;
    info.width = hdr.width;
    info.height = hdr.height;
    info.bits_per_sample = 8;
    info.sample_type = SampleType::U8;
    info.page_count = 1;

    if (hdr.bit_count <= 8u) {
        const bool is_gray = bmp_impl::is_gray_ramp_palette(bytes, hdr);
        info.channels = is_gray ? 1 : 3;
        info.has_palette = !is_gray;
    } else if (hdr.bit_count == 32u) {
        info.channels = 4;
    } else {
        info.channels = 3;
    }

    return info;
}

Result<Image> decode_bmp(ArrayRef<u8> bytes, const ImageDecodeOptions& opts) {
    KEEL_TRY_ASSIGN(const bmp_impl::BmpHeader hdr, bmp_impl::parse_bmp_header(bytes));

    const usize len = bytes.size();
    const u8* data = bytes.data();

    u8 pal_rgb[256][3] = {};
    usize pal_size = 0;
    bool is_palette_gray = false;

    if (hdr.bit_count <= 8u) {
        pal_size = (hdr.num_colors != 0) ? as<usize>(hdr.num_colors) : (usize{1} << hdr.bit_count);
        if (pal_size > 256u) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        const usize pal_off = 14u + as<usize>(hdr.header_size);
        const usize entry_bytes = (hdr.header_size == 12u) ? 3u : 4u;
        if (len < pal_off || len - pal_off < pal_size * entry_bytes) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        for (usize i = 0; i < pal_size; ++i) {
            const usize off = pal_off + i * entry_bytes;
            pal_rgb[i][2] = data[off + 0u]; // B
            pal_rgb[i][1] = data[off + 1u]; // G
            pal_rgb[i][0] = data[off + 2u]; // R
        }
        if (hdr.bit_count == 8u && pal_size == 256u) {
            is_palette_gray = true;
            for (usize i = 0; i < 256u; ++i) {
                if (pal_rgb[i][0] != i || pal_rgb[i][1] != i || pal_rgb[i][2] != i) {
                    is_palette_gray = false;
                    break;
                }
            }
        }
    }

    const u16 out_channels = (hdr.bit_count <= 8u) ? (is_palette_gray ? 1u : 3u) : ((hdr.bit_count == 32u) ? 4u : 3u);
    KEEL_TRY_ASSIGN(const usize out_bytes,
                    detail::validate_decode_allocation(hdr.width, hdr.height, out_channels, SampleType::U8, opts));

    if (len < as<usize>(hdr.off_bits)) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    const u8* pixel_data = data + hdr.off_bits;
    const usize pixel_data_len = len - as<usize>(hdr.off_bits);
    const usize w = as<usize>(hdr.width);
    const usize h = as<usize>(hdr.height);

    Allocator* dst_alloc = opts.allocator ? opts.allocator : default_allocator();
    Image img;
    img.width = hdr.width;
    img.height = hdr.height;
    img.channels = out_channels;
    img.sample_type = SampleType::U8;
    img.palette_bits = 0;
    img.pixels = Array<u8>(out_bytes, u8{0}, dst_alloc);
    img.palette = Array<u16>(dst_alloc);
    u8* dst = img.pixels.data();

    // BI_RLE8
    if (hdr.compression == 1u) {
        if (hdr.bit_count != 8u) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        ArenaTemp scratch = get_scratch(dst_alloc);
        Array<u8> indices(w * h, u8{0}, scratch.allocator());

        usize x = 0;
        usize y = 0;
        usize p = 0;
        while (p + 1u < pixel_data_len && y < h) {
            const u8 count = pixel_data[p++];
            const u8 val = pixel_data[p++];
            if (count > 0) {
                for (usize k = 0; k < count && x < w; ++k) {
                    indices[y * w + (x++)] = val;
                }
            } else if (val == 0u) {
                x = 0;
                ++y;
            } else if (val == 1u) {
                break;
            } else if (val == 2u) {
                if (p + 1u >= pixel_data_len) break;
                x += pixel_data[p++];
                y += pixel_data[p++];
            } else {
                const usize run = val;
                for (usize k = 0; k < run && p < pixel_data_len && x < w; ++k) {
                    indices[y * w + (x++)] = pixel_data[p++];
                }
                if ((run & 1u) != 0u && p < pixel_data_len) {
                    ++p;
                }
            }
        }

        for (usize r = 0; r < h; ++r) {
            const usize src_y = hdr.top_down ? r : (h - 1u - r);
            const u8* srow = indices.data() + src_y * w;
            u8* drow = dst + r * w * as<usize>(out_channels);
            if (is_palette_gray) {
                mem_copy(drow, srow, w);
            } else {
                for (usize c = 0; c < w; ++c) {
                    const u8 idx = srow[c];
                    if (idx < pal_size) {
                        drow[c * 3u + 0u] = pal_rgb[idx][0];
                        drow[c * 3u + 1u] = pal_rgb[idx][1];
                        drow[c * 3u + 2u] = pal_rgb[idx][2];
                    }
                }
            }
        }
        return img;
    }

    if (hdr.compression != 0u && hdr.compression != 3u) [[unlikely]] {
        return ErrorCode::NotSupported;
    }

    const usize row_stride = ((w * as<usize>(hdr.bit_count) + 31u) / 32u) * 4u;
    if (row_stride > 0 && h > Limits<usize>::kMax / row_stride) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }
    if (pixel_data_len < row_stride * h) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    for (usize r = 0; r < h; ++r) {
        const usize src_y = hdr.top_down ? r : (h - 1u - r);
        const u8* srow = pixel_data + src_y * row_stride;
        u8* drow = dst + r * w * as<usize>(out_channels);

        if (hdr.bit_count == 8u && is_palette_gray) {
            mem_copy(drow, srow, w);
        } else if (hdr.bit_count <= 8u) {
            for (usize c = 0; c < w; ++c) {
                u8 idx = 0;
                if (hdr.bit_count == 8u) {
                    idx = srow[c];
                } else if (hdr.bit_count == 4u) {
                    const u8 b = srow[c >> 1];
                    idx = ((c & 1u) == 0u) ? (b >> 4) : (b & 0x0Fu);
                } else {
                    const u8 b = srow[c >> 3];
                    idx = as<u8>((b >> (7u - (c & 7u))) & 0x01u);
                }
                if (idx < pal_size) {
                    drow[c * 3u + 0u] = pal_rgb[idx][0];
                    drow[c * 3u + 1u] = pal_rgb[idx][1];
                    drow[c * 3u + 2u] = pal_rgb[idx][2];
                }
            }
        } else if (hdr.bit_count == 24u) {
            for (usize c = 0; c < w; ++c) {
                drow[c * 3u + 0u] = srow[c * 3u + 2u];
                drow[c * 3u + 1u] = srow[c * 3u + 1u];
                drow[c * 3u + 2u] = srow[c * 3u + 0u];
            }
        } else if (hdr.bit_count == 32u) {
            for (usize c = 0; c < w; ++c) {
                drow[c * 4u + 0u] = srow[c * 4u + 2u];
                drow[c * 4u + 1u] = srow[c * 4u + 1u];
                drow[c * 4u + 2u] = srow[c * 4u + 0u];
                drow[c * 4u + 3u] = srow[c * 4u + 3u];
            }
        } else if (hdr.bit_count == 16u) {
            for (usize c = 0; c < w; ++c) {
                const u16 v = detail::read_u16_le(srow + c * 2u);
                drow[c * 3u + 0u] = as<u8>((((v >> 10) & 0x1Fu) * 255u) / 31u);
                drow[c * 3u + 1u] = as<u8>((((v >> 5) & 0x1Fu) * 255u) / 31u);
                drow[c * 3u + 2u] = as<u8>(((v & 0x1Fu) * 255u) / 31u);
            }
        }
    }

    return img;
}

Result<Array<u8>> encode_bmp(const ImageView& view, Allocator* alloc) {
    KEEL_TRY_ASSIGN(const detail::ValidatedView vv, detail::validate_image_view(view));
    if (view.sample_type != SampleType::U8) [[unlikely]] {
        return ErrorCode::NotSupported;
    }
    if (view.channels != 1 && view.channels != 3 && view.channels != 4) [[unlikely]] {
        return ErrorCode::NotSupported;
    }
    if (view.width > 0x7FFFFFFFu || view.height > 0x7FFFFFFFu) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }

    const usize w = as<usize>(view.width);
    const usize h = as<usize>(view.height);
    const u16 bit_count = (view.channels == 1) ? 8u : ((view.channels == 3) ? 24u : 32u);
    const usize row_stride = ((w * as<usize>(bit_count) + 31u) / 32u) * 4u;
    const usize pal_bytes = (view.channels == 1) ? 1024u : 0u;
    const usize off_bits = 14u + 40u + pal_bytes;

    if (row_stride > 0 && h > (usize{0xFFFFFFFFu} - off_bits) / row_stride) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }
    const usize image_size = row_stride * h;
    const usize file_size = off_bits + image_size;

    Allocator* dst_alloc = alloc ? alloc : default_allocator();
    Array<u8> out(dst_alloc);
    out.reserve(file_size);

    // 1. BITMAPFILEHEADER (14 bytes)
    out.push_back('B');
    out.push_back('M');
    detail::push_u32_le(out, as<u32>(file_size));
    detail::push_u16_le(out, 0u);
    detail::push_u16_le(out, 0u);
    detail::push_u32_le(out, as<u32>(off_bits));

    // 2. BITMAPINFOHEADER (40 bytes)
    detail::push_u32_le(out, 40u);
    detail::push_u32_le(out, view.width);
    detail::push_u32_le(out, view.height);
    detail::push_u16_le(out, 1u);
    detail::push_u16_le(out, bit_count);
    detail::push_u32_le(out, 0u);
    detail::push_u32_le(out, as<u32>(image_size));
    detail::push_u32_le(out, 2835u);
    detail::push_u32_le(out, 2835u);
    detail::push_u32_le(out, (view.channels == 1) ? 256u : 0u);
    detail::push_u32_le(out, 0u);

    // 3. Grayscale identity palette for 8-bit 1-channel BMP
    if (view.channels == 1) {
        for (u32 i = 0; i < 256u; ++i) {
            out.push_back(as<u8>(i));
            out.push_back(as<u8>(i));
            out.push_back(as<u8>(i));
            out.push_back(0u);
        }
    }

    // 4. Bottom-up pixel rows
    ArenaTemp scratch = get_scratch(dst_alloc);
    Array<u8> row_buf(row_stride, u8{0}, scratch.allocator());

    for (usize i = 0; i < h; ++i) {
        const usize r = h - 1u - i;
        const u8* srow = view.pixels.data() + r * vv.stride_bytes;
        mem_zero(row_buf.data(), row_stride);

        if (view.channels == 1) {
            mem_copy(row_buf.data(), srow, w);
        } else if (view.channels == 3) {
            for (usize c = 0; c < w; ++c) {
                row_buf[c * 3u + 0u] = srow[c * 3u + 2u];
                row_buf[c * 3u + 1u] = srow[c * 3u + 1u];
                row_buf[c * 3u + 2u] = srow[c * 3u + 0u];
            }
        } else {
            for (usize c = 0; c < w; ++c) {
                row_buf[c * 4u + 0u] = srow[c * 4u + 2u];
                row_buf[c * 4u + 1u] = srow[c * 4u + 1u];
                row_buf[c * 4u + 2u] = srow[c * 4u + 0u];
                row_buf[c * 4u + 3u] = srow[c * 4u + 3u];
            }
        }
        out.append(row_buf);
    }

    return out;
}

} // namespace keel::imagefile
