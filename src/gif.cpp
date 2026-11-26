#include "image_internal.h"

namespace keel::imagefile {

namespace {
namespace gif_impl {

struct GifFrameDesc {
    u32 left = 0;
    u32 top = 0;
    u32 width = 0;
    u32 height = 0;
    bool interlaced = false;
    u8 disposal = 0;
    bool has_trans = false;
    u8 trans_idx = 0;
    const u8* palette_rgb = nullptr;
    usize palette_colors = 0;
    usize lzw_offset = 0;
};

struct GifStream {
    u32 canvas_w = 0;
    u32 canvas_h = 0;
    u8 bg_color_idx = 0;
    const u8* gct_ptr = nullptr;
    usize gct_colors = 0;
    Array<GifFrameDesc> frames;
};

[[nodiscard]] Result<GifStream> parse_gif_stream(ArrayRef<u8> bytes, Allocator* alloc) {
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

    GifStream stream;
    stream.frames = Array<GifFrameDesc>(alloc);
    stream.canvas_w = detail::read_u16_le(data + 6u);
    stream.canvas_h = detail::read_u16_le(data + 8u);
    const u8 lsd_packed = data[10u];
    stream.bg_color_idx = data[11u];
    usize pos = 13u;

    if ((lsd_packed & 0x80u) != 0u) {
        stream.gct_colors = usize{2} << (lsd_packed & 0x07u);
        if (len - pos < 3u * stream.gct_colors) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        stream.gct_ptr = data + pos;
        pos += 3u * stream.gct_colors;
    }

    u8 pending_disposal = 0;
    bool pending_has_trans = false;
    u8 pending_trans_idx = 0;

    while (pos < len) {
        const u8 sep = data[pos++];
        if (sep == 0x3Bu) {
            break; // GIF Trailer
        }
        if (sep == 0x00u) {
            continue; // Tolerate zero padding between blocks
        }
        if (sep == 0x21u) {
            if (pos >= len) [[unlikely]] {
                if (!stream.frames.empty()) break;
                return ErrorCode::EndOfFile;
            }
            const u8 label = data[pos++];
            if (label == 0xF9u) {
                // Graphic Control Extension
                if (pos >= len) [[unlikely]] {
                    if (!stream.frames.empty()) break;
                    return ErrorCode::EndOfFile;
                }
                const u8 blk_size = data[pos++];
                if (len - pos < blk_size) [[unlikely]] {
                    if (!stream.frames.empty()) break;
                    return ErrorCode::EndOfFile;
                }
                if (blk_size >= 4u) {
                    const u8 gce_packed = data[pos];
                    pending_disposal = as<u8>((gce_packed >> 2) & 0x07u);
                    pending_has_trans = (gce_packed & 0x01u) != 0u;
                    pending_trans_idx = data[pos + 3u];
                }
                pos += blk_size;
                while (pos < len) {
                    const u8 sub_len = data[pos++];
                    if (sub_len == 0u) break;
                    if (len - pos < sub_len) [[unlikely]] {
                        if (!stream.frames.empty()) {
                            pos = len;
                            break;
                        }
                        return ErrorCode::EndOfFile;
                    }
                    pos += sub_len;
                }
            } else {
                while (pos < len) {
                    const u8 sub_len = data[pos++];
                    if (sub_len == 0u) break;
                    if (len - pos < sub_len) [[unlikely]] {
                        if (!stream.frames.empty()) {
                            pos = len;
                            break;
                        }
                        return ErrorCode::EndOfFile;
                    }
                    pos += sub_len;
                }
            }
            continue;
        }
        if (sep == 0x2Cu) {
            if (len - pos < 9u) [[unlikely]] {
                if (!stream.frames.empty()) break;
                return ErrorCode::EndOfFile;
            }
            GifFrameDesc frame;
            frame.left = detail::read_u16_le(data + pos + 0u);
            frame.top = detail::read_u16_le(data + pos + 2u);
            frame.width = detail::read_u16_le(data + pos + 4u);
            frame.height = detail::read_u16_le(data + pos + 6u);
            const u8 id_packed = data[pos + 8u];
            frame.interlaced = (id_packed & 0x40u) != 0u;
            frame.disposal = pending_disposal;
            frame.has_trans = pending_has_trans;
            frame.trans_idx = pending_trans_idx;
            pending_disposal = 0;
            pending_has_trans = false;
            pending_trans_idx = 0;
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
                frame.palette_rgb = stream.gct_ptr;
                frame.palette_colors = stream.gct_colors;
            }

            frame.lzw_offset = pos;
            if (pos >= len) [[unlikely]] {
                return ErrorCode::EndOfFile;
            }
            ++pos; // LZW minimum code size byte
            while (pos < len) {
                const u8 sub_len = data[pos++];
                if (sub_len == 0u) break;
                if (len - pos < sub_len) [[unlikely]] {
                    return ErrorCode::EndOfFile;
                }
                pos += sub_len;
            }

            stream.frames.push_back(frame);
            if (stream.frames.size() > 65536u) [[unlikely]] {
                return ErrorCode::OutOfRange;
            }
            continue;
        }
        if (!stream.frames.empty()) {
            break;
        }
        return ErrorCode::InvalidData;
    }

    if (stream.frames.empty()) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    // Clamp canvas dimensions to at least frame 0 if Logical Screen Descriptor was 0.
    if (stream.canvas_w == 0) stream.canvas_w = stream.frames[0].left + stream.frames[0].width;
    if (stream.canvas_h == 0) stream.canvas_h = stream.frames[0].top + stream.frames[0].height;
    if (stream.canvas_w == 0 || stream.canvas_h == 0) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    return stream;
}

[[nodiscard]] Result<Array<u8>> decode_gif_frame_indices(
    ArrayRef<u8> bytes, const GifFrameDesc& frame, const ImageDecodeOptions& opts, Allocator* dst_alloc,
    Arena& scratch_arena) {
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

    ArenaTemp frame_tmp = scratch_arena.begin_temp();
    Array<u8> compressed(frame_tmp.allocator());
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

    i16* prefix = frame_tmp.arena().alloc_array<i16>(4096);
    u8* suffix = frame_tmp.arena().alloc_array<u8>(4096);
    u8* first_char = frame_tmp.arena().alloc_array<u8>(4096);
    u8* stack = frame_tmp.arena().alloc_array<u8>(4096);

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

    Array<u8> decoded_indices(npix, u8{0}, frame.interlaced ? frame_tmp.allocator() : dst_alloc);
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

    if (!frame.interlaced) {
        return decoded_indices;
    }

    Array<u8> deinterlaced(npix, u8{0}, dst_alloc);
    const usize w = as<usize>(frame.width);
    const usize h = as<usize>(frame.height);
    constexpr usize kStart[4] = {0, 4, 2, 1};
    constexpr usize kStep[4] = {8, 8, 4, 2};
    usize src_row = 0;
    for (i32 pass = 0; pass < 4; ++pass) {
        for (usize r = kStart[pass]; r < h; r += kStep[pass]) {
            if (src_row < h) {
                mem_copy(deinterlaced.data() + r * w, decoded_indices.data() + src_row * w, w);
                ++src_row;
            }
        }
    }
    return deinterlaced;
}

} // namespace gif_impl
} // namespace

Result<ImageInfo> peek_gif(ArrayRef<u8> bytes) {
    ArenaTemp scratch = get_scratch();
    KEEL_TRY_ASSIGN(const gif_impl::GifStream stream, gif_impl::parse_gif_stream(bytes, scratch.allocator()));
    const gif_impl::GifFrameDesc& f0 = stream.frames[0];

    bool any_trans = false;
    for (usize i = 0; i < stream.frames.size(); ++i) {
        if (stream.frames[i].has_trans) {
            any_trans = true;
            break;
        }
    }

    ImageInfo info;
    info.format = ImageFormat::Gif;
    info.width = stream.canvas_w;
    info.height = stream.canvas_h;
    info.channels = any_trans ? 4 : 1;
    info.bits_per_sample = 8;
    info.sample_type = SampleType::U8;
    info.has_palette = (f0.palette_colors > 0);
    info.page_count = as<u32>(stream.frames.size());
    return info;
}

Result<Image> decode_gif(ArrayRef<u8> bytes, const ImageDecodeOptions& opts) {
    Allocator* dst_alloc = opts.allocator ? opts.allocator : default_allocator();
    ArenaTemp scratch = get_scratch(dst_alloc);

    KEEL_TRY_ASSIGN(const gif_impl::GifStream stream, gif_impl::parse_gif_stream(bytes, scratch.allocator()));
    if (opts.page >= stream.frames.size()) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }

    const gif_impl::GifFrameDesc& f0 = stream.frames[0];
    const bool f0_covers_canvas = (f0.left == 0u && f0.top == 0u && f0.width == stream.canvas_w &&
                                   f0.height == stream.canvas_h);

    // Direct indexed path for opaque full-canvas frame 0 when expand_indexed is false.
    if (opts.page == 0u && !opts.expand_indexed && !f0.has_trans && f0_covers_canvas) {
        KEEL_TRY_ASSIGN(Array<u8> indices,
                        gif_impl::decode_gif_frame_indices(bytes, f0, opts, dst_alloc, scratch.arena()));

        Image img;
        img.width = f0.width;
        img.height = f0.height;
        img.channels = 1;
        img.sample_type = SampleType::U8;
        img.pixels = keel::move(indices);

        if (f0.palette_colors > 0 && f0.palette_rgb != nullptr) {
            img.palette = Array<u16>(f0.palette_colors * 3u, u16{0}, dst_alloc);
            for (usize i = 0; i < f0.palette_colors * 3u; ++i) {
                img.palette[i] = as<u16>(f0.palette_rgb[i]);
            }
            img.palette_bits = 8;
        } else {
            img.palette = Array<u16>(dst_alloc);
            img.palette_bits = 0;
        }
        return img;
    }

    // Multi-frame / transparency / sub-rect / expand_indexed compositing path.
    KEEL_TRY_ASSIGN(const usize canvas_rgba_bytes,
                    detail::validate_decode_allocation(stream.canvas_w, stream.canvas_h, 4u, SampleType::U8, opts));

    const usize cw = as<usize>(stream.canvas_w);
    const usize ch = as<usize>(stream.canvas_h);
    Allocator* work_alloc = (canvas_rgba_bytes <= 8ULL * 1024 * 1024) ? scratch.allocator() : nullptr;

    Array<u8> canvas(canvas_rgba_bytes, u8{0}, work_alloc);
    Array<u8> prev_canvas(work_alloc);
    bool any_transparency = !f0_covers_canvas;

    for (u32 fi = 0; fi <= opts.page; ++fi) {
        const gif_impl::GifFrameDesc& frame = stream.frames[fi];
        if (frame.has_trans) {
            any_transparency = true;
        }

        const bool save_prev = (fi < opts.page && frame.disposal == 3u);
        if (save_prev) {
            prev_canvas.resize(canvas_rgba_bytes);
            mem_copy(prev_canvas.data(), canvas.data(), canvas_rgba_bytes);
        }

        ArenaTemp frame_arena_tmp = scratch.arena().begin_temp();
        KEEL_TRY_ASSIGN(Array<u8> indices,
                        gif_impl::decode_gif_frame_indices(bytes, frame, opts, frame_arena_tmp.allocator(),
                                                           scratch.arena()));

        const usize fw = as<usize>(frame.width);
        const usize fh = as<usize>(frame.height);
        const usize fl = as<usize>(frame.left);
        const usize ft = as<usize>(frame.top);
        const u8* pal = frame.palette_rgb;
        const usize pal_colors = frame.palette_colors;
        const u8* idx_ptr = indices.data();
        u8* dst = canvas.data();

        for (usize py = 0; py < fh; ++py) {
            const usize cy = ft + py;
            if (cy >= ch) break;
            for (usize px = 0; px < fw; ++px) {
                const usize cx = fl + px;
                if (cx >= cw) break;
                const u8 idx = idx_ptr[py * fw + px];
                if (frame.has_trans && idx == frame.trans_idx) {
                    continue;
                }
                const usize off = (cy * cw + cx) * 4u;
                if (pal != nullptr && as<usize>(idx) < pal_colors) {
                    dst[off + 0u] = pal[as<usize>(idx) * 3u + 0u];
                    dst[off + 1u] = pal[as<usize>(idx) * 3u + 1u];
                    dst[off + 2u] = pal[as<usize>(idx) * 3u + 2u];
                } else {
                    dst[off + 0u] = 0u;
                    dst[off + 1u] = 0u;
                    dst[off + 2u] = 0u;
                }
                dst[off + 3u] = 255u;
            }
        }

        if (fi == opts.page) {
            break;
        }

        if (frame.disposal == 2u) {
            any_transparency = true;
            for (usize py = 0; py < fh; ++py) {
                const usize cy = ft + py;
                if (cy >= ch) break;
                for (usize px = 0; px < fw; ++px) {
                    const usize cx = fl + px;
                    if (cx >= cw) break;
                    const usize off = (cy * cw + cx) * 4u;
                    dst[off + 0u] = 0u;
                    dst[off + 1u] = 0u;
                    dst[off + 2u] = 0u;
                    dst[off + 3u] = 0u;
                }
            }
        } else if (frame.disposal == 3u && prev_canvas.size() == canvas_rgba_bytes) {
            mem_copy(canvas.data(), prev_canvas.data(), canvas_rgba_bytes);
        }
    }

    const usize npix = cw * ch;
    bool has_transparent_pixel = false;
    const u8* cptr = canvas.data();
    for (usize i = 0; i < npix; ++i) {
        if (cptr[i * 4u + 3u] != 255u) {
            has_transparent_pixel = true;
            break;
        }
    }

    const bool emit_rgba = any_transparency || has_transparent_pixel;
    Image img;
    img.width = stream.canvas_w;
    img.height = stream.canvas_h;
    img.channels = emit_rgba ? 4u : 3u;
    img.sample_type = SampleType::U8;
    img.palette = Array<u16>(dst_alloc);
    img.palette_bits = 0;

    if (emit_rgba) {
        img.pixels = Array<u8>(canvas_rgba_bytes, u8{0}, dst_alloc);
        mem_copy(img.pixels.data(), canvas.data(), canvas_rgba_bytes);
    } else {
        img.pixels = Array<u8>(npix * 3u, u8{0}, dst_alloc);
        u8* dp = img.pixels.data();
        for (usize i = 0; i < npix; ++i) {
            dp[i * 3u + 0u] = cptr[i * 4u + 0u];
            dp[i * 3u + 1u] = cptr[i * 4u + 1u];
            dp[i * 3u + 2u] = cptr[i * 4u + 2u];
        }
    }
    return img;
}

} // namespace keel::imagefile
