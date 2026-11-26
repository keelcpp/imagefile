#include "image_internal.h"

#include "keel/compress.h"

namespace keel::imagefile {

namespace {
namespace png_impl {

constexpr u8 kPngSignature[8] = {0x89u, 0x50u, 0x4Eu, 0x47u, 0x0Du, 0x0Au, 0x1Au, 0x0Au};

struct PngIhdr {
    u32 width = 0;
    u32 height = 0;
    u8 bit_depth = 0;
    u8 color_type = 0;
    u8 compression = 0;
    u8 filter = 0;
    u8 interlace = 0;
};

[[nodiscard]] constexpr i32 paeth_predictor(i32 a, i32 b, i32 c) noexcept {
    const i32 p = a + b - c;
    const i32 pa = (p >= a) ? (p - a) : (a - p);
    const i32 pb = (p >= b) ? (p - b) : (b - p);
    const i32 pc = (p >= c) ? (p - c) : (c - p);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

[[nodiscard]] constexpr u16 color_type_channels(u8 color_type) noexcept {
    switch (color_type) {
        case 0:
            return 1; // Grayscale
        case 2:
            return 3; // RGB
        case 3:
            return 1; // Indexed
        case 4:
            return 2; // Grayscale + Alpha
        case 6:
            return 4; // RGBA
        default:
            return 0;
    }
}

[[nodiscard]] Result<PngIhdr> parse_ihdr(ArrayRef<u8> bytes) noexcept {
    const usize len = bytes.size();
    const u8* data = bytes.data();
    if (len < 8) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    if (!mem_eq(data, kPngSignature, 8)) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (len < 8 + 12 + 13) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    const u32 chunk_len = detail::read_u32_be(data + 8);
    if (chunk_len != 13u || !mem_eq(data + 12, "IHDR", 4)) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    const u32 expected_crc = detail::read_u32_be(data + 8 + 4 + 4 + 13);
    const u32 actual_crc = keel::compress::crc32(ArrayRef<u8>(data + 12, 4 + 13));
    if (expected_crc != actual_crc) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    PngIhdr ihdr;
    ihdr.width = detail::read_u32_be(data + 16);
    ihdr.height = detail::read_u32_be(data + 20);
    ihdr.bit_depth = data[24];
    ihdr.color_type = data[25];
    ihdr.compression = data[26];
    ihdr.filter = data[27];
    ihdr.interlace = data[28];

    if (ihdr.width == 0 || ihdr.height == 0 || ihdr.width > 0x7FFFFFFFu || ihdr.height > 0x7FFFFFFFu) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (ihdr.compression != 0 || ihdr.filter != 0 || (ihdr.interlace != 0 && ihdr.interlace != 1)) [[unlikely]] {
        return ErrorCode::NotSupported;
    }

    if (ihdr.color_type == 0) {
        if (ihdr.bit_depth != 1 && ihdr.bit_depth != 2 && ihdr.bit_depth != 4 && ihdr.bit_depth != 8 &&
            ihdr.bit_depth != 16) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
    } else if (ihdr.color_type == 2 || ihdr.color_type == 4 || ihdr.color_type == 6) {
        if (ihdr.bit_depth != 8 && ihdr.bit_depth != 16) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
    } else if (ihdr.color_type == 3) {
        if (ihdr.bit_depth != 1 && ihdr.bit_depth != 2 && ihdr.bit_depth != 4 && ihdr.bit_depth != 8) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
    } else {
        return ErrorCode::InvalidData;
    }

    return ihdr;
}

[[nodiscard]] ErrorCode unfilter_pass(
    const u8* raw, usize pass_h, usize row_bytes, usize safe_bpp, u8* dst_unfiltered) noexcept {
    for (usize r = 0; r < pass_h; ++r) {
        const usize src_off = r * (row_bytes + 1u);
        const u8 filter_type = raw[src_off];
        const u8* filt = raw + src_off + 1u;
        u8* curr = dst_unfiltered + r * row_bytes;
        const u8* prev = (r > 0) ? (dst_unfiltered + (r - 1u) * row_bytes) : nullptr;

        switch (filter_type) {
            case 0: // None
                mem_copy(curr, filt, row_bytes);
                break;
            case 1: // Sub
                for (usize x = 0; x < row_bytes; ++x) {
                    const u8 left = (x >= safe_bpp) ? curr[x - safe_bpp] : 0u;
                    curr[x] = as<u8>(filt[x] + left);
                }
                break;
            case 2: // Up
                for (usize x = 0; x < row_bytes; ++x) {
                    const u8 up = prev ? prev[x] : 0u;
                    curr[x] = as<u8>(filt[x] + up);
                }
                break;
            case 3: // Average
                for (usize x = 0; x < row_bytes; ++x) {
                    const i32 left = (x >= safe_bpp) ? as<i32>(curr[x - safe_bpp]) : 0;
                    const i32 up = prev ? as<i32>(prev[x]) : 0;
                    curr[x] = as<u8>(as<i32>(filt[x]) + ((left + up) >> 1));
                }
                break;
            case 4: // Paeth
                for (usize x = 0; x < row_bytes; ++x) {
                    const i32 left = (x >= safe_bpp) ? as<i32>(curr[x - safe_bpp]) : 0;
                    const i32 up = prev ? as<i32>(prev[x]) : 0;
                    const i32 up_left = (prev && x >= safe_bpp) ? as<i32>(prev[x - safe_bpp]) : 0;
                    curr[x] = as<u8>(as<i32>(filt[x]) + paeth_predictor(left, up, up_left));
                }
                break;
            default:
                return ErrorCode::InvalidData;
        }
    }
    return ErrorCode::Ok;
}

void scatter_pass_pixels(const u8* pass_rows, usize pass_w, usize pass_h, usize row_bytes, usize x0, usize dx, usize y0,
                         usize dy, const PngIhdr& ihdr, u16 channels, u8* dst_pixels) noexcept {
    const usize full_w = as<usize>(ihdr.width);
    const u8 bd = ihdr.bit_depth;

    if (dx == 1u && y0 == 0u && dy == 1u && bd == 8u && ihdr.color_type != 3u) {
        mem_copy(dst_pixels, pass_rows, pass_h * row_bytes);
        return;
    }

    for (usize py = 0; py < pass_h; ++py) {
        const usize r = y0 + py * dy;
        const u8* srow = pass_rows + py * row_bytes;
        for (usize px = 0; px < pass_w; ++px) {
            const usize c = x0 + px * dx;
            const usize dst_pix_idx = (r * full_w + c) * as<usize>(channels);

            if (bd == 16u) {
                for (u16 ch = 0; ch < channels; ++ch) {
                    const usize soff = (px * as<usize>(channels) + ch) * 2u;
                    const u16 val = detail::read_u16_be(srow + soff);
                    detail::write_u16_native(dst_pixels + (dst_pix_idx + ch) * 2u, val);
                }
            } else if (bd == 8u) {
                for (u16 ch = 0; ch < channels; ++ch) {
                    dst_pixels[dst_pix_idx + ch] = srow[px * as<usize>(channels) + ch];
                }
            } else {
                // Sub-byte 1, 2, or 4-bit (color_type == 0 or 3, channels == 1)
                u8 raw_sample = 0;
                if (bd == 4u) {
                    const u8 b = srow[px >> 1];
                    raw_sample = ((px & 1u) == 0u) ? (b >> 4) : (b & 0x0Fu);
                } else if (bd == 2u) {
                    const u8 b = srow[px >> 2];
                    raw_sample = as<u8>((b >> (6u - 2u * (px & 3u))) & 0x03u);
                } else {
                    const u8 b = srow[px >> 3];
                    raw_sample = as<u8>((b >> (7u - (px & 7u))) & 0x01u);
                }

                if (ihdr.color_type == 3u) {
                    dst_pixels[dst_pix_idx] = raw_sample;
                } else {
                    const u32 max_val = (1u << bd) - 1u;
                    dst_pixels[dst_pix_idx] = as<u8>((as<u32>(raw_sample) * 255u) / max_val);
                }
            }
        }
    }
}

void append_chunk(Array<u8>& out, const char type[4], const u8* payload, usize payload_len) {
    detail::push_u32_be(out, as<u32>(payload_len));
    const usize type_pos = out.size();
    out.push_back(as<u8>(type[0]));
    out.push_back(as<u8>(type[1]));
    out.push_back(as<u8>(type[2]));
    out.push_back(as<u8>(type[3]));
    if (payload_len > 0 && payload != nullptr) {
        out.append(ArrayRef<u8>(payload, payload_len));
    }
    const u32 c = keel::compress::crc32(out.slice(type_pos, 4u + payload_len));
    detail::push_u32_be(out, c);
}

} // namespace png_impl
} // namespace

Result<ImageInfo> peek_png(ArrayRef<u8> bytes) {
    KEEL_TRY_ASSIGN(const png_impl::PngIhdr ihdr, png_impl::parse_ihdr(bytes));

    u16 channels = png_impl::color_type_channels(ihdr.color_type);
    if (ihdr.color_type == 0u || ihdr.color_type == 2u) {
        const u8* data = bytes.data();
        const usize len = bytes.size();
        usize offset = 8u;
        while (offset + 12u <= len) {
            const usize chunk_len = as<usize>(detail::read_u32_be(data + offset));
            if (len - offset - 12u < chunk_len) {
                break;
            }
            const u8* chunk_type = data + offset + 4u;
            if (mem_eq(chunk_type, "tRNS", 4)) {
                const usize expected_len = (ihdr.color_type == 0u) ? 2u : 6u;
                if (chunk_len == expected_len) {
                    channels = as<u16>(channels + 1u);
                }
                break;
            }
            if (mem_eq(chunk_type, "IDAT", 4) || mem_eq(chunk_type, "IEND", 4)) {
                break;
            }
            offset += 12u + chunk_len;
        }
    }

    ImageInfo info;
    info.format = ImageFormat::Png;
    info.width = ihdr.width;
    info.height = ihdr.height;
    info.channels = channels;
    info.bits_per_sample = ihdr.bit_depth;
    info.sample_type = (ihdr.bit_depth == 16u) ? SampleType::U16 : SampleType::U8;
    info.has_palette = (ihdr.color_type == 3u);
    info.page_count = 1;
    return info;
}

Result<Image> decode_png(ArrayRef<u8> bytes, const ImageDecodeOptions& opts) {
    KEEL_TRY_ASSIGN(const png_impl::PngIhdr ihdr, png_impl::parse_ihdr(bytes));

    const u16 channels = png_impl::color_type_channels(ihdr.color_type);
    const SampleType sample_type = (ihdr.bit_depth == 16u) ? SampleType::U16 : SampleType::U8;
    KEEL_TRY_ASSIGN(const usize base_out_bytes,
                    detail::validate_decode_allocation(ihdr.width, ihdr.height, channels, sample_type, opts));

    Allocator* dst_alloc = opts.allocator ? opts.allocator : default_allocator();
    ArenaTemp scratch = get_scratch(dst_alloc);

    Array<u8> idat_stream(scratch.allocator());
    Array<u16> palette(dst_alloc);
    Array<u8> trns_alpha(scratch.allocator());
    bool saw_plte = false;
    bool saw_trns = false;
    u16 trns_key[3] = {0, 0, 0};
    bool saw_iend = false;

    const u8* data = bytes.data();
    const usize len = bytes.size();
    usize offset = 8u;

    while (offset < len) {
        if (len - offset < 12u) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        const u32 chunk_len_u32 = detail::read_u32_be(data + offset);
        const usize chunk_len = as<usize>(chunk_len_u32);
        if (len - offset - 12u < chunk_len) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }

        const u8* chunk_type = data + offset + 4u;
        const u8* chunk_data = data + offset + 8u;
        const u32 expected_crc = detail::read_u32_be(data + offset + 8u + chunk_len);
        const u32 actual_crc = keel::compress::crc32(ArrayRef<u8>(chunk_type, 4u + chunk_len));
        if (expected_crc != actual_crc) [[unlikely]] {
            return ErrorCode::InvalidData;
        }

        if (mem_eq(chunk_type, "PLTE", 4)) {
            if (chunk_len < 3u || chunk_len > 768u || (chunk_len % 3u) != 0u) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            const usize num_entries = chunk_len / 3u;
            palette.resize(num_entries * 3u);
            for (usize i = 0; i < num_entries * 3u; ++i) {
                palette[i] = as<u16>(chunk_data[i]);
            }
            saw_plte = true;
        } else if (mem_eq(chunk_type, "tRNS", 4)) {
            if (saw_trns) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            if (ihdr.color_type == 0u) {
                if (chunk_len != 2u) [[unlikely]] {
                    return ErrorCode::InvalidData;
                }
                trns_key[0] = detail::read_u16_be(chunk_data);
                saw_trns = true;
            } else if (ihdr.color_type == 2u) {
                if (chunk_len != 6u) [[unlikely]] {
                    return ErrorCode::InvalidData;
                }
                trns_key[0] = detail::read_u16_be(chunk_data + 0u);
                trns_key[1] = detail::read_u16_be(chunk_data + 2u);
                trns_key[2] = detail::read_u16_be(chunk_data + 4u);
                saw_trns = true;
            } else if (ihdr.color_type == 3u) {
                if (!saw_plte || chunk_len > palette.size() / 3u) [[unlikely]] {
                    return ErrorCode::InvalidData;
                }
                trns_alpha.resize(chunk_len);
                if (chunk_len > 0) {
                    mem_copy(trns_alpha.data(), chunk_data, chunk_len);
                }
                saw_trns = true;
            } else {
                // tRNS is forbidden for color types that already have an alpha channel (4, 6).
                return ErrorCode::InvalidData;
            }
        } else if (mem_eq(chunk_type, "IDAT", 4)) {
            idat_stream.append(ArrayRef<u8>(chunk_data, chunk_len));
        } else if (mem_eq(chunk_type, "IEND", 4)) {
            if (chunk_len != 0u) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            saw_iend = true;
            break;
        }

        offset += 12u + chunk_len;
    }

    if (!saw_iend) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    if (idat_stream.empty()) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (ihdr.color_type == 3u && !saw_plte) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    const bool apply_key_trns = saw_trns && (ihdr.color_type == 0u || ihdr.color_type == 2u);
    const bool apply_pal_trns = saw_trns && ihdr.color_type == 3u && opts.expand_indexed;
    if (apply_key_trns) {
        KEEL_TRY(detail::validate_decode_allocation(ihdr.width, ihdr.height, as<u16>(channels + 1u), sample_type, opts));
    } else if (apply_pal_trns) {
        KEEL_TRY(detail::validate_decode_allocation(ihdr.width, ihdr.height, 4u, SampleType::U8, opts));
    }

    const usize w = as<usize>(ihdr.width);
    const usize h = as<usize>(ihdr.height);
    const usize bits_per_pixel = as<usize>(channels) * as<usize>(ihdr.bit_depth);
    const usize safe_bpp = (bits_per_pixel >= 8u) ? (bits_per_pixel / 8u) : 1u;

    constexpr usize kX0[7] = {0, 4, 0, 2, 0, 1, 0};
    constexpr usize kDX[7] = {8, 8, 4, 4, 2, 2, 1};
    constexpr usize kY0[7] = {0, 0, 4, 0, 2, 0, 1};
    constexpr usize kDY[7] = {8, 8, 8, 4, 4, 2, 2};

    const i32 num_passes = (ihdr.interlace == 1u) ? 7 : 1;
    usize expected_raw_bytes = 0;
    usize max_pass_unfiltered = 0;

    for (i32 p = 0; p < num_passes; ++p) {
        const usize x0 = (num_passes == 1) ? 0u : kX0[p];
        const usize dx = (num_passes == 1) ? 1u : kDX[p];
        const usize y0 = (num_passes == 1) ? 0u : kY0[p];
        const usize dy = (num_passes == 1) ? 1u : kDY[p];

        const usize pw = (w > x0) ? ((w - x0 + dx - 1u) / dx) : 0u;
        const usize ph = (h > y0) ? ((h - y0 + dy - 1u) / dy) : 0u;
        if (pw == 0u || ph == 0u) continue;

        const usize row_bytes = (pw * bits_per_pixel + 7u) / 8u;
        if (row_bytes + 1u > Limits<usize>::kMax / ph) [[unlikely]] {
            return ErrorCode::OutOfRange;
        }
        const usize pass_raw = (row_bytes + 1u) * ph;
        if (expected_raw_bytes > Limits<usize>::kMax - pass_raw) [[unlikely]] {
            return ErrorCode::OutOfRange;
        }
        expected_raw_bytes += pass_raw;
        max_pass_unfiltered = keel::max(max_pass_unfiltered, row_bytes * ph);
    }

    if (opts.max_bytes > 0 && expected_raw_bytes > opts.max_bytes + h * 8u + 64u) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }

    keel::compress::InflateOptions inf_opts;
    inf_opts.expected_size_hint = expected_raw_bytes;
    inf_opts.max_output_bytes = (opts.max_bytes > 0) ? (expected_raw_bytes + 1024u) : 0u;
    inf_opts.allocator = scratch.allocator();

    KEEL_TRY_ASSIGN(Array<u8> raw_scanlines, keel::compress::zlib_decompress(idat_stream, inf_opts));
    if (raw_scanlines.size() < expected_raw_bytes) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    const bool need_post_expand = apply_key_trns || (opts.expand_indexed && ihdr.color_type == 3u);
    Allocator* base_alloc = need_post_expand ? scratch.allocator() : dst_alloc;

    Image img;
    img.width = ihdr.width;
    img.height = ihdr.height;
    img.channels = channels;
    img.sample_type = sample_type;
    img.pixels = Array<u8>(base_out_bytes, u8{0}, base_alloc);
    if (ihdr.color_type == 3u) {
        img.palette = keel::move(palette);
        img.palette_bits = 8;
    } else {
        img.palette = Array<u16>(dst_alloc);
        img.palette_bits = 0;
    }

    Array<u8> pass_unfiltered(max_pass_unfiltered, u8{0}, scratch.allocator());
    usize raw_pos = 0;

    for (i32 p = 0; p < num_passes; ++p) {
        const usize x0 = (num_passes == 1) ? 0u : kX0[p];
        const usize dx = (num_passes == 1) ? 1u : kDX[p];
        const usize y0 = (num_passes == 1) ? 0u : kY0[p];
        const usize dy = (num_passes == 1) ? 1u : kDY[p];

        const usize pw = (w > x0) ? ((w - x0 + dx - 1u) / dx) : 0u;
        const usize ph = (h > y0) ? ((h - y0 + dy - 1u) / dy) : 0u;
        if (pw == 0u || ph == 0u) continue;

        const usize row_bytes = (pw * bits_per_pixel + 7u) / 8u;
        const ErrorCode ec = png_impl::unfilter_pass(raw_scanlines.data() + raw_pos, ph, row_bytes, safe_bpp,
                                                     pass_unfiltered.data());
        if (ec != ErrorCode::Ok) [[unlikely]] {
            return ec;
        }
        png_impl::scatter_pass_pixels(pass_unfiltered.data(), pw, ph, row_bytes, x0, dx, y0, dy, ihdr, channels,
                                      img.pixels.data());
        raw_pos += (row_bytes + 1u) * ph;
    }

    const usize npix = w * h;

    // Apply tRNS color-key transparency for grayscale (type 0 -> GA) and RGB (type 2 -> RGBA).
    if (apply_key_trns) {
        const u16 out_ch = as<u16>(channels + 1u);
        const usize bps = (sample_type == SampleType::U16) ? 2u : 1u;
        Image out;
        out.width = ihdr.width;
        out.height = ihdr.height;
        out.channels = out_ch;
        out.sample_type = sample_type;
        out.palette = Array<u16>(dst_alloc);
        out.palette_bits = 0;
        out.pixels = Array<u8>(npix * as<usize>(out_ch) * bps, u8{0}, dst_alloc);

        const u8* sp = img.pixels.data();
        u8* dp = out.pixels.data();

        if (ihdr.color_type == 0u) {
            if (sample_type == SampleType::U16) {
                for (usize i = 0; i < npix; ++i) {
                    const u16 g = detail::read_u16_native(sp + i * 2u);
                    const u16 a = (g == trns_key[0]) ? u16{0} : u16{65535};
                    detail::write_u16_native(dp + i * 4u + 0u, g);
                    detail::write_u16_native(dp + i * 4u + 2u, a);
                }
            } else {
                const u32 max_raw = (1u << ihdr.bit_depth) - 1u;
                const bool key_valid = (as<u32>(trns_key[0]) <= max_raw);
                const u8 key_scaled =
                    key_valid ? as<u8>((as<u32>(trns_key[0]) * 255u) / max_raw) : u8{0};
                for (usize i = 0; i < npix; ++i) {
                    const u8 g = sp[i];
                    dp[i * 2u + 0u] = g;
                    dp[i * 2u + 1u] = (key_valid && g == key_scaled) ? u8{0} : u8{255};
                }
            }
        } else {
            if (sample_type == SampleType::U16) {
                for (usize i = 0; i < npix; ++i) {
                    const u16 r = detail::read_u16_native(sp + i * 6u + 0u);
                    const u16 g = detail::read_u16_native(sp + i * 6u + 2u);
                    const u16 b = detail::read_u16_native(sp + i * 6u + 4u);
                    const bool match = (r == trns_key[0] && g == trns_key[1] && b == trns_key[2]);
                    detail::write_u16_native(dp + i * 8u + 0u, r);
                    detail::write_u16_native(dp + i * 8u + 2u, g);
                    detail::write_u16_native(dp + i * 8u + 4u, b);
                    detail::write_u16_native(dp + i * 8u + 6u, match ? u16{0} : u16{65535});
                }
            } else {
                const bool key_valid = (trns_key[0] <= 255u && trns_key[1] <= 255u && trns_key[2] <= 255u);
                const u8 kr = as<u8>(trns_key[0] & 0xFFu);
                const u8 kg = as<u8>(trns_key[1] & 0xFFu);
                const u8 kb = as<u8>(trns_key[2] & 0xFFu);
                for (usize i = 0; i < npix; ++i) {
                    const u8 r = sp[i * 3u + 0u];
                    const u8 g = sp[i * 3u + 1u];
                    const u8 b = sp[i * 3u + 2u];
                    const bool match = (key_valid && r == kr && g == kg && b == kb);
                    dp[i * 4u + 0u] = r;
                    dp[i * 4u + 1u] = g;
                    dp[i * 4u + 2u] = b;
                    dp[i * 4u + 3u] = match ? u8{0} : u8{255};
                }
            }
        }
        return out;
    }

    // Apply PLTE + tRNS indexed RGBA expansion when expand_indexed is requested.
    if (apply_pal_trns && img.has_palette()) {
        Image out;
        out.width = ihdr.width;
        out.height = ihdr.height;
        out.channels = 4u;
        out.sample_type = SampleType::U8;
        out.palette = Array<u16>(dst_alloc);
        out.palette_bits = 0;
        out.pixels = Array<u8>(npix * 4u, u8{0}, dst_alloc);

        const usize pal_entries = img.palette.size() / 3u;
        const u16* pal = img.palette.data();
        const u8* sp = img.pixels.data();
        u8* dp = out.pixels.data();
        for (usize i = 0; i < npix; ++i) {
            const usize idx = as<usize>(sp[i]);
            if (idx < pal_entries) {
                dp[i * 4u + 0u] = as<u8>(pal[idx * 3u + 0u] & 0xFFu);
                dp[i * 4u + 1u] = as<u8>(pal[idx * 3u + 1u] & 0xFFu);
                dp[i * 4u + 2u] = as<u8>(pal[idx * 3u + 2u] & 0xFFu);
            }
            dp[i * 4u + 3u] = (idx < trns_alpha.size()) ? trns_alpha[idx] : u8{255};
        }
        return out;
    }

    if (opts.expand_indexed && img.has_palette()) {
        return expand_palette(img, dst_alloc);
    }
    return img;
}

Result<Array<u8>> encode_png(const ImageView& view, const PngEncodeOptions& opts) {
    KEEL_TRY_ASSIGN(const detail::ValidatedView vv, detail::validate_image_view(view));
    if (view.width > 0x7FFFFFFFu || view.height > 0x7FFFFFFFu) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }

    const bool has_palette = !opts.palette.empty();
    if (has_palette) {
        if (view.channels != 1 || view.sample_type != SampleType::U8) [[unlikely]] {
            return ErrorCode::InvalidArgument;
        }
        if (opts.palette.size() % 3u != 0u || opts.palette.size() > 768u) [[unlikely]] {
            return ErrorCode::InvalidArgument;
        }
    }

    if (view.sample_type != SampleType::U8 && view.sample_type != SampleType::U16) [[unlikely]] {
        return ErrorCode::NotSupported;
    }

    u8 color_type = 0;
    if (has_palette) {
        color_type = 3u;
    } else if (view.channels == 1) {
        color_type = 0u;
    } else if (view.channels == 2) {
        color_type = 4u;
    } else if (view.channels == 3) {
        color_type = 2u;
    } else if (view.channels == 4) {
        color_type = 6u;
    } else {
        return ErrorCode::NotSupported;
    }

    const bool is_16bit = (view.sample_type == SampleType::U16);
    const u8 bit_depth = is_16bit ? 16u : 8u;
    const usize w = as<usize>(view.width);
    const usize h = as<usize>(view.height);
    const usize row_bytes = vv.tight_row_bytes;
    const usize bpp = as<usize>(view.channels) * (is_16bit ? 2u : 1u);

    if (row_bytes + 1u > Limits<usize>::kMax / h) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }

    Allocator* dst_alloc = opts.allocator ? opts.allocator : default_allocator();
    ArenaTemp scratch = get_scratch(dst_alloc);

    Array<u8> filtered(scratch.allocator());
    filtered.reserve((row_bytes + 1u) * h);
    Array<u8> curr_row(row_bytes, u8{0}, scratch.allocator());

    for (usize r = 0; r < h; ++r) {
        const u8* src_row = view.pixels.data() + r * vv.stride_bytes;
        if (!is_16bit) {
            mem_copy(curr_row.data(), src_row, row_bytes);
        } else {
            const usize num_samples = w * as<usize>(view.channels);
            for (usize s = 0; s < num_samples; ++s) {
                const u16 val = detail::read_u16_native(src_row + s * 2u);
                curr_row[s * 2u] = as<u8>((val >> 8) & 0xFFu);
                curr_row[s * 2u + 1u] = as<u8>(val & 0xFFu);
            }
        }

        if (has_palette) {
            filtered.push_back(0u); // Filter None preserves exact index locality
            filtered.append(curr_row);
        } else {
            filtered.push_back(1u); // Filter Sub
            const u8* row_ptr = curr_row.data();
            for (usize x = 0; x < row_bytes; ++x) {
                const u8 left = (x >= bpp) ? row_ptr[x - bpp] : 0u;
                filtered.push_back(as<u8>(row_ptr[x] - left));
            }
        }
    }

    const i32 level = keel::clamp(opts.compression_level, 0, 9);
    Array<u8> compressed = keel::compress::zlib_compress(filtered, level, scratch.allocator());

    Array<u8> out(dst_alloc);
    out.reserve(8u + 25u + (has_palette ? (12u + opts.palette.size()) : 0u) + 12u + compressed.size() + 12u);
    out.append(ArrayRef<u8>(png_impl::kPngSignature, 8));

    u8 ihdr_payload[13] = {};
    ihdr_payload[0] = as<u8>((view.width >> 24) & 0xFFu);
    ihdr_payload[1] = as<u8>((view.width >> 16) & 0xFFu);
    ihdr_payload[2] = as<u8>((view.width >> 8) & 0xFFu);
    ihdr_payload[3] = as<u8>(view.width & 0xFFu);
    ihdr_payload[4] = as<u8>((view.height >> 24) & 0xFFu);
    ihdr_payload[5] = as<u8>((view.height >> 16) & 0xFFu);
    ihdr_payload[6] = as<u8>((view.height >> 8) & 0xFFu);
    ihdr_payload[7] = as<u8>(view.height & 0xFFu);
    ihdr_payload[8] = bit_depth;
    ihdr_payload[9] = color_type;
    ihdr_payload[10] = 0u;
    ihdr_payload[11] = 0u;
    ihdr_payload[12] = 0u;
    png_impl::append_chunk(out, "IHDR", ihdr_payload, 13u);

    if (has_palette) {
        Array<u8> plte_bytes(opts.palette.size(), u8{0}, scratch.allocator());
        for (usize i = 0; i < opts.palette.size(); ++i) {
            plte_bytes[i] = as<u8>(keel::min<u16>(opts.palette[i], 255u));
        }
        png_impl::append_chunk(out, "PLTE", plte_bytes.data(), plte_bytes.size());
    }

    png_impl::append_chunk(out, "IDAT", compressed.data(), compressed.size());
    png_impl::append_chunk(out, "IEND", nullptr, 0u);

    return out;
}

} // namespace keel::imagefile
