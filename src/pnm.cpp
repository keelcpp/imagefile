#include "image_internal.h"

namespace keel::imagefile {

namespace {
namespace pnm_impl {

[[nodiscard]] constexpr bool is_space(u8 c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

[[nodiscard]] constexpr bool is_digit(u8 c) noexcept { return c >= '0' && c <= '9'; }

[[nodiscard]] usize skip_ws_and_comments(const u8* data, usize len, usize p) noexcept {
    while (p < len) {
        if (is_space(data[p])) {
            ++p;
        } else if (data[p] == '#') {
            while (p < len && data[p] != '\n' && data[p] != '\r') {
                ++p;
            }
        } else {
            break;
        }
    }
    return p;
}

[[nodiscard]] ErrorCode read_next_u32(const u8* data, usize len, usize& p, u32& val) noexcept {
    p = skip_ws_and_comments(data, len, p);
    if (p >= len) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    if (!is_digit(data[p])) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    u64 acc = 0;
    while (p < len && is_digit(data[p])) {
        acc = acc * 10u + static_cast<u64>(data[p] - '0');
        if (acc > 0xFFFFFFFFULL) [[unlikely]] {
            return ErrorCode::OutOfRange;
        }
        ++p;
    }
    val = static_cast<u32>(acc);
    return ErrorCode::Ok;
}

struct PnmHeader {
    int magic = 0; // 1..6
    u32 width = 0;
    u32 height = 0;
    u32 maxval = 255;
    usize data_offset = 0;
};

[[nodiscard]] Result<PnmHeader> parse_pnm_header(ArrayRef<u8> bytes) noexcept {
    const usize len = bytes.size();
    const u8* data = bytes.data();
    if (len < 3) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    if (data[0] != 'P' || data[1] < '1' || data[1] > '6' || (!is_space(data[2]) && data[2] != '#')) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    PnmHeader hdr;
    hdr.magic = static_cast<int>(data[1] - '0');
    usize p = 2u;

    if (const ErrorCode ec = read_next_u32(data, len, p, hdr.width); ec != ErrorCode::Ok) return ec;
    if (const ErrorCode ec = read_next_u32(data, len, p, hdr.height); ec != ErrorCode::Ok) return ec;

    if (hdr.magic != 1 && hdr.magic != 4) {
        if (const ErrorCode ec = read_next_u32(data, len, p, hdr.maxval); ec != ErrorCode::Ok) return ec;
        if (hdr.maxval == 0 || hdr.maxval > 65535u) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
    } else {
        hdr.maxval = 1;
    }

    if (hdr.width == 0 || hdr.height == 0) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    // Per Netpbm spec: after the last header token, skip any comment and consume ONE whitespace byte
    // so binary P4/P5/P6 payloads whose first pixel byte equals ' ', '\n', or '#' are never eaten.
    while (p < len && data[p] == '#') {
        while (p < len && data[p] != '\n' && data[p] != '\r') {
            ++p;
        }
    }
    if (hdr.magic >= 4) {
        if (p >= len) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        if (!is_space(data[p])) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        ++p;
    }
    hdr.data_offset = p;
    return hdr;
}

void push_u32_ascii(Array<u8>& buf, u32 v) {
    u8 digits[12];
    int n = 0;
    if (v == 0) {
        buf.push_back('0');
        return;
    }
    while (v > 0) {
        digits[n++] = static_cast<u8>('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        buf.push_back(digits[--n]);
    }
}

} // namespace pnm_impl
} // namespace

Result<ImageInfo> peek_pnm(ArrayRef<u8> bytes) {
    KEEL_TRY_ASSIGN(const pnm_impl::PnmHeader hdr, pnm_impl::parse_pnm_header(bytes));

    ImageInfo info;
    info.format = ImageFormat::Pnm;
    info.width = hdr.width;
    info.height = hdr.height;
    info.channels = (hdr.magic == 3 || hdr.magic == 6) ? 3 : 1;
    info.bits_per_sample = (hdr.maxval > 255u) ? 16 : ((hdr.magic == 1 || hdr.magic == 4) ? 1 : 8);
    info.sample_type = (hdr.maxval > 255u) ? SampleType::U16 : SampleType::U8;
    info.has_palette = false;
    info.page_count = 1;
    return info;
}

Result<Image> decode_pnm(ArrayRef<u8> bytes, const ImageDecodeOptions& opts) {
    KEEL_TRY_ASSIGN(const pnm_impl::PnmHeader hdr, pnm_impl::parse_pnm_header(bytes));

    const bool is_16bit = (hdr.maxval > 255u);
    const bool is_color = (hdr.magic == 3 || hdr.magic == 6);
    const bool is_binary = (hdr.magic >= 4);
    const u16 channels = is_color ? 3 : 1;
    const SampleType sample_type = is_16bit ? SampleType::U16 : SampleType::U8;

    KEEL_TRY_ASSIGN(const usize out_bytes,
                    detail::validate_decode_allocation(hdr.width, hdr.height, channels, sample_type, opts));

    const usize len = bytes.size();
    const u8* data = bytes.data();
    const usize w = static_cast<usize>(hdr.width);
    const usize h = static_cast<usize>(hdr.height);

    Allocator* dst_alloc = opts.allocator ? opts.allocator : default_allocator();
    Image img;
    img.width = hdr.width;
    img.height = hdr.height;
    img.channels = channels;
    img.sample_type = sample_type;
    img.palette_bits = 0;
    img.pixels = Array<u8>(out_bytes, u8{0}, dst_alloc);
    img.palette = Array<u16>(dst_alloc);
    u8* dst = img.pixels.data();

    if (is_binary) {
        if (hdr.magic == 4) {
            const usize row_bytes = (w + 7u) / 8u;
            if (len - hdr.data_offset < row_bytes * h) [[unlikely]] {
                return ErrorCode::EndOfFile;
            }
            for (usize r = 0; r < h; ++r) {
                const u8* srow = data + hdr.data_offset + r * row_bytes;
                u8* drow = dst + r * w;
                for (usize c = 0; c < w; ++c) {
                    const u8 bit = static_cast<u8>((srow[c >> 3] >> (7u - (c & 7u))) & 1u);
                    drow[c] = bit ? 0u : 255u;
                }
            }
            return img;
        }

        const usize total_samples = w * h * static_cast<usize>(channels);
        const usize src_bytes = total_samples * (is_16bit ? 2u : 1u);
        if (len - hdr.data_offset < src_bytes) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        const u8* src = data + hdr.data_offset;
        if (!is_16bit) {
            if (hdr.maxval == 255u) {
                mem_copy(dst, src, total_samples);
            } else {
                for (usize i = 0; i < total_samples; ++i) {
                    const u32 v = (src[i] > hdr.maxval) ? hdr.maxval : static_cast<u32>(src[i]);
                    dst[i] = static_cast<u8>((v * 255u + (hdr.maxval >> 1)) / hdr.maxval);
                }
            }
        } else {
            for (usize i = 0; i < total_samples; ++i) {
                u32 v = detail::read_u16_be(src + i * 2u);
                if (v > hdr.maxval) v = hdr.maxval;
                if (hdr.maxval != 65535u) {
                    v = static_cast<u32>((static_cast<u64>(v) * 65535ULL + (hdr.maxval >> 1)) / hdr.maxval);
                }
                detail::write_u16_native(dst + i * 2u, static_cast<u16>(v));
            }
        }
        return img;
    }

    // ASCII formats: P1, P2, P3
    usize p = hdr.data_offset;
    const usize total_samples = w * h * static_cast<usize>(channels);
    for (usize i = 0; i < total_samples; ++i) {
        u32 val = 0;
        const ErrorCode ec = pnm_impl::read_next_u32(data, len, p, val);
        if (ec != ErrorCode::Ok) [[unlikely]] {
            return ec;
        }
        if (hdr.magic == 1) {
            if (val > 1u) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            dst[i] = (val != 0u) ? 0u : 255u;
        } else if (!is_16bit) {
            if (val > hdr.maxval) val = hdr.maxval;
            dst[i] = (hdr.maxval == 255u) ? static_cast<u8>(val)
                                          : static_cast<u8>((val * 255u + (hdr.maxval >> 1)) / hdr.maxval);
        } else {
            if (val > hdr.maxval) val = hdr.maxval;
            const u16 v16 = (hdr.maxval == 65535u)
                                ? static_cast<u16>(val)
                                : static_cast<u16>((static_cast<u64>(val) * 65535ULL + (hdr.maxval >> 1)) / hdr.maxval);
            detail::write_u16_native(dst + i * 2u, v16);
        }
    }

    return img;
}

Result<Array<u8>> encode_pnm(const ImageView& view, const PnmEncodeOptions& opts) {
    KEEL_TRY_ASSIGN(const detail::ValidatedView vv, detail::validate_image_view(view));
    if (view.sample_type != SampleType::U8 && view.sample_type != SampleType::U16) [[unlikely]] {
        return ErrorCode::NotSupported;
    }

    PnmSubformat sub = opts.subformat;
    if (sub == PnmSubformat::Auto) {
        if (view.channels == 1) {
            sub = PnmSubformat::Pgm;
        } else if (view.channels == 3) {
            sub = PnmSubformat::Ppm;
        } else {
            return ErrorCode::NotSupported;
        }
    }

    if (sub == PnmSubformat::Pbm || sub == PnmSubformat::Pgm) {
        if (view.channels != 1) [[unlikely]] {
            return ErrorCode::NotSupported;
        }
    } else if (sub == PnmSubformat::Ppm) {
        if (view.channels != 3) [[unlikely]] {
            return ErrorCode::NotSupported;
        }
    } else {
        return ErrorCode::InvalidArgument;
    }

    const usize w = static_cast<usize>(view.width);
    const usize h = static_cast<usize>(view.height);
    const bool is_16bit = (view.sample_type == SampleType::U16);
    Allocator* dst_alloc = opts.allocator ? opts.allocator : default_allocator();

    Array<u8> out(dst_alloc);

    if (sub == PnmSubformat::Pbm) {
        const usize row_bytes = (w + 7u) / 8u;
        out.reserve(32u + row_bytes * h);
        out.push_back('P');
        out.push_back('4');
        out.push_back('\n');
        pnm_impl::push_u32_ascii(out, view.width);
        out.push_back(' ');
        pnm_impl::push_u32_ascii(out, view.height);
        out.push_back('\n');

        const usize hdr_len = out.size();
        out.resize(hdr_len + row_bytes * h, u8{0});
        u8* dst = out.data() + hdr_len;

        for (usize r = 0; r < h; ++r) {
            const u8* srow = view.pixels.data() + r * vv.stride_bytes;
            u8* drow = dst + r * row_bytes;
            for (usize c = 0; c < w; ++c) {
                bool is_black = false;
                if (!is_16bit) {
                    is_black = (srow[c] < 128u);
                } else {
                    is_black = (detail::read_u16_native(srow + c * 2u) < 32768u);
                }
                if (is_black) {
                    drow[c >> 3] |= static_cast<u8>(1u << (7u - (c & 7u)));
                }
            }
        }
        return out;
    }

    const u8 magic_digit = (sub == PnmSubformat::Pgm) ? '5' : '6';
    const u32 maxval = is_16bit ? 65535u : 255u;
    const usize payload_bytes = w * h * static_cast<usize>(view.channels) * (is_16bit ? 2u : 1u);

    out.reserve(40u + payload_bytes);
    out.push_back('P');
    out.push_back(magic_digit);
    out.push_back('\n');
    pnm_impl::push_u32_ascii(out, view.width);
    out.push_back(' ');
    pnm_impl::push_u32_ascii(out, view.height);
    out.push_back('\n');
    pnm_impl::push_u32_ascii(out, maxval);
    out.push_back('\n');

    const usize hdr_len = out.size();
    out.resize(hdr_len + payload_bytes);
    u8* dst = out.data() + hdr_len;

    if (!is_16bit) {
        for (usize r = 0; r < h; ++r) {
            mem_copy(dst + r * vv.tight_row_bytes, view.pixels.data() + r * vv.stride_bytes, vv.tight_row_bytes);
        }
    } else {
        const usize row_samples = w * static_cast<usize>(view.channels);
        for (usize r = 0; r < h; ++r) {
            const u8* srow = view.pixels.data() + r * vv.stride_bytes;
            u8* drow = dst + r * row_samples * 2u;
            for (usize s = 0; s < row_samples; ++s) {
                const u16 v = detail::read_u16_native(srow + s * 2u);
                drow[s * 2u] = static_cast<u8>((v >> 8) & 0xFFu);
                drow[s * 2u + 1u] = static_cast<u8>(v & 0xFFu);
            }
        }
    }

    return out;
}

} // namespace keel::imagefile
