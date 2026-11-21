#pragma once

#include "keel/imagefile.h"

#include "keel/arena.h"
#include "keel/bits.h"

namespace keel::imagefile::detail {

[[nodiscard]] inline bool is_host_big_endian() noexcept {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return true;
#else
    return false;
#endif
}

[[nodiscard]] constexpr u16 read_u16_le(const u8* p) noexcept {
    return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8));
}

[[nodiscard]] constexpr u16 read_u16_be(const u8* p) noexcept {
    return static_cast<u16>((static_cast<u16>(p[0]) << 8) | static_cast<u16>(p[1]));
}

[[nodiscard]] constexpr u32 read_u32_le(const u8* p) noexcept {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
           (static_cast<u32>(p[3]) << 24);
}

[[nodiscard]] constexpr u32 read_u32_be(const u8* p) noexcept {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) |
           static_cast<u32>(p[3]);
}

[[nodiscard]] constexpr u64 read_u64_le(const u8* p) noexcept {
    return static_cast<u64>(read_u32_le(p)) | (static_cast<u64>(read_u32_le(p + 4)) << 32);
}

[[nodiscard]] constexpr u64 read_u64_be(const u8* p) noexcept {
    return (static_cast<u64>(read_u32_be(p)) << 32) | static_cast<u64>(read_u32_be(p + 4));
}

inline void push_u16_le(Array<u8>& buf, u16 v) {
    buf.push_back(static_cast<u8>(v & 0xFFu));
    buf.push_back(static_cast<u8>((v >> 8) & 0xFFu));
}

inline void push_u16_be(Array<u8>& buf, u16 v) {
    buf.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<u8>(v & 0xFFu));
}

inline void push_u32_le(Array<u8>& buf, u32 v) {
    buf.push_back(static_cast<u8>(v & 0xFFu));
    buf.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<u8>((v >> 24) & 0xFFu));
}

inline void push_u32_be(Array<u8>& buf, u32 v) {
    buf.push_back(static_cast<u8>((v >> 24) & 0xFFu));
    buf.push_back(static_cast<u8>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<u8>((v >> 8) & 0xFFu));
    buf.push_back(static_cast<u8>(v & 0xFFu));
}

inline void push_u64_le(Array<u8>& buf, u64 v) {
    push_u32_le(buf, static_cast<u32>(v & 0xFFFFFFFFULL));
    push_u32_le(buf, static_cast<u32>((v >> 32) & 0xFFFFFFFFULL));
}

inline void write_u16_native(u8* dst, u16 v) noexcept { mem_copy(dst, &v, 2); }
inline u16 read_u16_native(const u8* src) noexcept {
    u16 v = 0;
    mem_copy(&v, src, 2);
    return v;
}

inline void write_u32_native(u8* dst, u32 v) noexcept { mem_copy(dst, &v, 4); }
inline u32 read_u32_native(const u8* src) noexcept {
    u32 v = 0;
    mem_copy(&v, src, 4);
    return v;
}

inline void write_u64_native(u8* dst, u64 v) noexcept { mem_copy(dst, &v, 8); }
inline u64 read_u64_native(const u8* src) noexcept {
    u64 v = 0;
    mem_copy(&v, src, 8);
    return v;
}

[[nodiscard]] inline Result<usize> validate_decode_allocation(
    u32 width, u32 height, u16 channels, SampleType sample_type, const ImageDecodeOptions& opts) noexcept {
    if (width == 0 || height == 0 || channels == 0) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if ((opts.max_width > 0 && width > opts.max_width) || (opts.max_height > 0 && height > opts.max_height))
        [[unlikely]] {
        return ErrorCode::OutOfRange;
    }

    const usize w = static_cast<usize>(width);
    const usize h = static_cast<usize>(height);
    if (w > (~usize{0}) / h) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }
    const usize num_pixels = w * h;
    if (opts.max_pixels > 0 && num_pixels > opts.max_pixels) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }

    const usize bps = sample_byte_size(sample_type);
    const usize pixel_bytes = static_cast<usize>(channels) * bps;
    if (pixel_bytes == 0 || num_pixels > (~usize{0}) / pixel_bytes) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }
    const usize total_bytes = num_pixels * pixel_bytes;
    if (opts.max_bytes > 0 && total_bytes > opts.max_bytes) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }

    return total_bytes;
}

struct ValidatedView {
    usize tight_row_bytes = 0;
    usize stride_bytes = 0;
};

[[nodiscard]] inline Result<ValidatedView> validate_image_view(const ImageView& view) noexcept {
    if (view.width == 0 || view.height == 0 || view.channels == 0 || view.pixels.data() == nullptr) [[unlikely]] {
        return ErrorCode::InvalidArgument;
    }
    const usize bps = sample_byte_size(view.sample_type);
    const usize pixel_bytes = static_cast<usize>(view.channels) * bps;
    if (pixel_bytes == 0 || static_cast<usize>(view.width) > (~usize{0}) / pixel_bytes) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }
    const usize tight_row = static_cast<usize>(view.width) * pixel_bytes;
    const usize stride = (view.row_stride_bytes != 0) ? view.row_stride_bytes : tight_row;
    if (stride < tight_row) [[unlikely]] {
        return ErrorCode::InvalidArgument;
    }

    const usize h_minus_1 = static_cast<usize>(view.height) - 1u;
    if (h_minus_1 > 0 && stride > ((~usize{0}) - tight_row) / h_minus_1) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }
    const usize required = h_minus_1 * stride + tight_row;
    if (view.pixels.size() < required) [[unlikely]] {
        return ErrorCode::InvalidArgument;
    }

    return ValidatedView{.tight_row_bytes = tight_row, .stride_bytes = stride};
}

} // namespace keel::imagefile::detail
