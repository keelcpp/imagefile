#include "image_internal.h"

namespace keel::imagefile {

namespace {
namespace gif_impl {

struct GifFirstFrame {
    u32 width = 0;
    u32 height = 0;
    bool interlaced = false;
    const u8* palette_rgb = nullptr;
    usize palette_colors = 0;
    usize lzw_offset = 0;
};

[[nodiscard]] Result<GifFirstFrame> locate_first_frame(ArrayRef<u8> bytes) noexcept {
    const usize len = bytes.size();
    const u8* data = bytes.data();
    if (len < 6) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    if (data[0] != 'G' || data[1] != 'I' || data[2] != 'F' || data[3] != '8' || (data[4] != '7' && data[4] != '9') ||
        data[5] != 'a') [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (len < 13) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    usize pos = 6u;
    const u8 lsd_packed = data[pos + 4u];
    pos += 7u;

    const u8* gct_ptr = nullptr;
    usize gct_colors = 0;
    if ((lsd_packed & 0x80u) != 0u) {
        gct_colors = usize{2} << (lsd_packed & 0x07u);
        if (len - pos < 3u * gct_colors) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        gct_ptr = data + pos;
        pos += 3u * gct_colors;
    }

    while (pos < len) {
        const u8 sep = data[pos++];
        if (sep == 0x3Bu) {
            return ErrorCode::InvalidData; // Trailer reached without an image frame
        }
        if (sep == 0x21u) {
            if (pos >= len) return ErrorCode::EndOfFile;
            ++pos; // Extension label
            while (pos < len) {
                const u8 sub_len = data[pos++];
                if (sub_len == 0u) break;
                if (len - pos < sub_len) return ErrorCode::EndOfFile;
                pos += sub_len;
            }
            continue;
        }
        if (sep == 0x2Cu) {
            if (len - pos < 9u) [[unlikely]] {
                return ErrorCode::EndOfFile;
            }
            GifFirstFrame frame;
            frame.width = detail::read_u16_le(data + pos + 4u);
            frame.height = detail::read_u16_le(data + pos + 6u);
            const u8 id_packed = data[pos + 8u];
            frame.interlaced = (id_packed & 0x40u) != 0u;
            pos += 9u;

            if (frame.width == 0 || frame.height == 0) [[unlikely]] {
                return ErrorCode::InvalidData;
            }

            if ((id_packed & 0x80u) != 0u) {
                const usize lct_colors = usize{2} << (id_packed & 0x07u);
                if (len - pos < 3u * lct_colors) [[unlikely]] {
                    return ErrorCode::EndOfFile;
                }
                frame.palette_rgb = data + pos;
                frame.palette_colors = lct_colors;
                pos += 3u * lct_colors;
            } else {
                frame.palette_rgb = gct_ptr;
                frame.palette_colors = gct_colors;
            }

            frame.lzw_offset = pos;
            return frame;
        }
        return ErrorCode::InvalidData;
    }

    return ErrorCode::EndOfFile;
}

} // namespace gif_impl
} // namespace

Result<ImageInfo> peek_gif(ArrayRef<u8> bytes) {
    KEEL_TRY_ASSIGN(const gif_impl::GifFirstFrame frame, gif_impl::locate_first_frame(bytes));

    ImageInfo info;
    info.format = ImageFormat::Gif;
    info.width = frame.width;
    info.height = frame.height;
    info.channels = 1;
    info.bits_per_sample = 8;
    info.sample_type = SampleType::U8;
    info.has_palette = (frame.palette_colors > 0);
    info.page_count = 1;
    return info;
}

Result<Image> decode_gif(ArrayRef<u8> bytes, const ImageDecodeOptions& opts) {
    KEEL_TRY_ASSIGN(const gif_impl::GifFirstFrame frame, gif_impl::locate_first_frame(bytes));
    KEEL_TRY_ASSIGN(const usize npix,
                    detail::validate_decode_allocation(frame.width, frame.height, 1, SampleType::U8, opts));

    const usize len = bytes.size();
    const u8* data = bytes.data();
    usize pos = frame.lzw_offset;
    if (pos >= len) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    const i32 min_code_size = as<i32>(data[pos++]);
    if (min_code_size < 2 || min_code_size > 8) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    Allocator* dst_alloc = opts.allocator ? opts.allocator : default_allocator();
    ArenaTemp scratch = get_scratch(dst_alloc);

    Array<u8> compressed(scratch.allocator());
    bool saw_terminator = false;
    while (pos < len) {
        const u8 sub_len = data[pos++];
        if (sub_len == 0u) {
            saw_terminator = true;
            break;
        }
        if (len - pos < sub_len) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        compressed.append(ArrayRef<u8>(data + pos, sub_len));
        pos += sub_len;
    }
    if (!saw_terminator && compressed.empty()) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    const i32 clear_code = 1 << min_code_size;
    const i32 end_code = clear_code + 1;
    i32 code_size = min_code_size + 1;
    i32 next_code = end_code + 1;

    i16* prefix = scratch.arena().alloc_array<i16>(4096);
    u8* suffix = scratch.arena().alloc_array<u8>(4096);
    u8* first_char = scratch.arena().alloc_array<u8>(4096);
    u8* stack = scratch.arena().alloc_array<u8>(4096);

    auto reset_table = [&]() noexcept {
        code_size = min_code_size + 1;
        next_code = end_code + 1;
        for (i32 i = 0; i < clear_code; ++i) {
            prefix[i] = -1;
            suffix[i] = as<u8>(i);
            first_char[i] = as<u8>(i);
        }
    };
    reset_table();

    Array<u8> decoded_indices(npix, u8{0}, frame.interlaced ? scratch.allocator() : dst_alloc);
    usize out_pos = 0;

    const u8* comp_ptr = compressed.data();
    const usize total_bits = compressed.size() * 8u;
    usize bit_pos = 0;
    i32 prev_code = -1;

    while (bit_pos + as<usize>(code_size) <= total_bits && out_pos < npix) {
        i32 code = 0;
        for (i32 b = 0; b < code_size; ++b) {
            const usize bp = bit_pos + as<usize>(b);
            if (((comp_ptr[bp >> 3] >> (bp & 7u)) & 1u) != 0u) {
                code |= (1 << b);
            }
        }
        bit_pos += as<usize>(code_size);

        if (code == clear_code) {
            reset_table();
            prev_code = -1;
            continue;
        }
        if (code == end_code) {
            break;
        }

        i32 cur = code;
        usize sp = 0;
        if (code == next_code && prev_code >= 0) {
            stack[sp++] = first_char[prev_code];
            cur = prev_code;
        } else if (code >= next_code) [[unlikely]] {
            return ErrorCode::InvalidData;
        }

        while (cur >= 0) {
            if (sp >= 4096u) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            stack[sp++] = suffix[cur];
            cur = prefix[cur];
        }

        const u8 first_byte = stack[sp - 1u];
        while (sp > 0 && out_pos < npix) {
            decoded_indices[out_pos++] = stack[--sp];
        }

        if (prev_code >= 0 && next_code < 4096) {
            prefix[next_code] = as<i16>(prev_code);
            suffix[next_code] = first_byte;
            first_char[next_code] = first_char[prev_code];
            ++next_code;
            if (next_code == (1 << code_size) && code_size < 12) {
                ++code_size;
            }
        }
        prev_code = code;
    }

    Image img;
    img.width = frame.width;
    img.height = frame.height;
    img.channels = 1;
    img.sample_type = SampleType::U8;

    if (!frame.interlaced) {
        img.pixels = keel::move(decoded_indices);
    } else {
        img.pixels = Array<u8>(npix, u8{0}, dst_alloc);
        const usize w = as<usize>(frame.width);
        const usize h = as<usize>(frame.height);
        constexpr usize kStart[4] = {0, 4, 2, 1};
        constexpr usize kStep[4] = {8, 8, 4, 2};
        usize src_row = 0;
        for (i32 pass = 0; pass < 4; ++pass) {
            for (usize r = kStart[pass]; r < h; r += kStep[pass]) {
                if (src_row < h) {
                    mem_copy(img.pixels.data() + r * w, decoded_indices.data() + src_row * w, w);
                    ++src_row;
                }
            }
        }
    }

    if (frame.palette_colors > 0 && frame.palette_rgb != nullptr) {
        img.palette = Array<u16>(frame.palette_colors * 3u, u16{0}, dst_alloc);
        for (usize i = 0; i < frame.palette_colors * 3u; ++i) {
            img.palette[i] = as<u16>(frame.palette_rgb[i]);
        }
        img.palette_bits = 8;
    } else {
        img.palette = Array<u16>(dst_alloc);
        img.palette_bits = 0;
    }

    if (opts.expand_indexed && img.has_palette()) {
        return expand_palette(img, dst_alloc);
    }
    return img;
}

} // namespace keel::imagefile
