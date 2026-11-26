#include "image_internal.h"

#include "keel/compress.h"

namespace keel::imagefile {

bool is_tiff_bytes(ArrayRef<u8> bytes) noexcept {
    if (bytes.size() < 4) return false;
    const u8* d = bytes.data();
    if (d[0] == 'I' && d[1] == 'I') {
        return (d[2] == 42u && d[3] == 0u) || (d[2] == 43u && d[3] == 0u);
    }
    if (d[0] == 'M' && d[1] == 'M') {
        return (d[2] == 0u && d[3] == 42u) || (d[2] == 0u && d[3] == 43u);
    }
    return false;
}

namespace {
namespace tiff_impl {

constexpr u32 kMaxIfds = 65536u;
constexpr usize kMaxTagEntriesPerIfd = 65535u;
constexpr u64 kMaxTagArrayCount = 16ULL * 1024 * 1024;

// TIFF 6.0 + BigTIFF tag value byte widths (indices 1..18).
constexpr usize kTypeWidth[19] = {
    0,
    1, // 1  BYTE
    1, // 2  ASCII
    2, // 3  SHORT
    4, // 4  LONG
    8, // 5  RATIONAL
    1, // 6  SBYTE
    1, // 7  UNDEFINED
    2, // 8  SSHORT
    4, // 9  SLONG
    8, // 10 SRATIONAL
    4, // 11 FLOAT
    8, // 12 DOUBLE
    4, // 13 IFD
    0, // 14 reserved
    0, // 15 reserved
    8, // 16 LONG8  (BigTIFF)
    8, // 17 SLONG8 (BigTIFF)
    8, // 18 IFD8   (BigTIFF)
};

struct ByteReader {
    const u8* buf = nullptr;
    usize size = 0;
    bool big_endian = false;
    bool is_big_tiff = false;

    [[nodiscard]] Result<void> check(usize off, usize n) const noexcept {
        if (off > size || n > size - off) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        return {};
    }

    [[nodiscard]] Result<u16> u16_at(usize off) const noexcept {
        KEEL_TRY(check(off, 2));
        return big_endian ? detail::read_u16_be(buf + off) : detail::read_u16_le(buf + off);
    }

    [[nodiscard]] Result<u32> u32_at(usize off) const noexcept {
        KEEL_TRY(check(off, 4));
        return big_endian ? detail::read_u32_be(buf + off) : detail::read_u32_le(buf + off);
    }

    [[nodiscard]] Result<u64> u64_at(usize off) const noexcept {
        KEEL_TRY(check(off, 8));
        return big_endian ? detail::read_u64_be(buf + off) : detail::read_u64_le(buf + off);
    }

    [[nodiscard]] Result<u64> offset_at(usize off) const noexcept {
        if (is_big_tiff) {
            return u64_at(off);
        }
        KEEL_TRY_ASSIGN(const u32 v, u32_at(off));
        return as<u64>(v);
    }
};

[[nodiscard]] Result<ByteReader> open_tiff(ArrayRef<u8> bytes) noexcept {
    if (bytes.size() < 8) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    const u8* d = bytes.data();
    bool be = false;
    if (d[0] == 'I' && d[1] == 'I') {
        be = false;
    } else if (d[0] == 'M' && d[1] == 'M') {
        be = true;
    } else {
        return ErrorCode::InvalidData;
    }

    ByteReader br{.buf = d, .size = bytes.size(), .big_endian = be, .is_big_tiff = false};
    KEEL_TRY_ASSIGN(const u16 magic, br.u16_at(2));
    if (magic == 42u) {
        return br;
    }
    if (magic == 43u) {
        if (bytes.size() < 16) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        KEEL_TRY_ASSIGN(const u16 off_size, br.u16_at(4));
        KEEL_TRY_ASSIGN(const u16 pad_zero, br.u16_at(6));
        if (off_size != 8u || pad_zero != 0u) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        br.is_big_tiff = true;
        return br;
    }
    return ErrorCode::InvalidData;
}

[[nodiscard]] Result<u64> first_ifd_offset(const ByteReader& br) noexcept {
    if (br.is_big_tiff) {
        return br.u64_at(8);
    }
    KEEL_TRY_ASSIGN(const u32 off32, br.u32_at(4));
    return as<u64>(off32);
}

[[nodiscard]] Result<usize> step_ifd(const ByteReader& br, u64 off, u64* next_off_out,
                                     usize* next_slot_out = nullptr) noexcept {
    if (off > as<u64>(br.size)) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    const usize ifd_off = as<usize>(off);
    const usize count_hdr = br.is_big_tiff ? 8u : 2u;
    const usize entry_size = br.is_big_tiff ? 20u : 12u;
    const usize next_ptr_size = br.is_big_tiff ? 8u : 4u;

    u64 entry_count = 0;
    if (br.is_big_tiff) {
        KEEL_TRY_ASSIGN(entry_count, br.u64_at(ifd_off));
    } else {
        KEEL_TRY_ASSIGN(const u16 c16, br.u16_at(ifd_off));
        entry_count = c16;
    }
    if (entry_count > kMaxTagEntriesPerIfd) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    const usize entries_bytes = as<usize>(entry_count) * entry_size;
    if (ifd_off > br.size - count_hdr || entries_bytes > br.size - ifd_off - count_hdr) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    const usize next_slot = ifd_off + count_hdr + entries_bytes;
    if (next_ptr_size > br.size - next_slot) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    KEEL_TRY_ASSIGN(const u64 next_off, br.offset_at(next_slot));
    if (next_off_out) *next_off_out = next_off;
    if (next_slot_out) *next_slot_out = next_slot;
    return as<usize>(entry_count);
}

[[nodiscard]] Result<usize> locate_ifd_for_page(const ByteReader& br, u32 page_zero_based) noexcept {
    KEEL_TRY_ASSIGN(u64 off, first_ifd_offset(br));
    if (off == 0) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    for (u32 p = 0;; ++p) {
        if (p >= kMaxIfds) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        if (off == 0) [[unlikely]] {
            return ErrorCode::OutOfRange;
        }
        u64 next_off = 0;
        KEEL_TRY(step_ifd(br, off, &next_off));
        if (p == page_zero_based) {
            return as<usize>(off);
        }
        off = next_off;
    }
}

[[nodiscard]] Result<ArenaArray<u64>> read_entry_values(const ByteReader& br, u16 type, u64 count, u64 value_offset,
                                                        usize entry_offset, Arena& arena) {
    if (type == 0 || type > 18 || kTypeWidth[type] == 0) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (count > kMaxTagArrayCount) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }
    const usize w = kTypeWidth[type];
    const usize cnt = as<usize>(count);
    const usize total = cnt * w;
    const usize inline_cap = br.is_big_tiff ? 8u : 4u;
    const usize inline_slot_off = br.is_big_tiff ? 12u : 8u;

    usize base = 0;
    if (total <= inline_cap) {
        base = entry_offset + inline_slot_off;
    } else {
        if (value_offset > as<u64>(br.size)) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        base = as<usize>(value_offset);
    }
    KEEL_TRY(br.check(base, total));

    ArenaArray<u64> out(arena, cnt);
    for (usize i = 0; i < cnt; ++i) {
        const usize off = base + i * w;
        switch (type) {
            case 1:
            case 2:
            case 6:
            case 7:
                out.push_back(br.buf[off]);
                break;
            case 3:
            case 8: {
                KEEL_TRY_ASSIGN(const u16 v, br.u16_at(off));
                out.push_back(v);
                break;
            }
            case 4:
            case 9:
            case 11:
            case 13: {
                KEEL_TRY_ASSIGN(const u32 v, br.u32_at(off));
                out.push_back(v);
                break;
            }
            case 16:
            case 17:
            case 18: {
                KEEL_TRY_ASSIGN(const u64 v, br.u64_at(off));
                out.push_back(v);
                break;
            }
            default:
                out.push_back(0);
                break;
        }
    }
    return out;
}

struct TiffIfdInfo {
    u32 width = 0;
    u32 height = 0;
    u16 bits_per_sample = 8;
    u16 compression = 1;
    u16 photometric = 1;
    u16 samples_per_pixel = 1;
    u32 rows_per_strip = 0;
    u16 planar_config = 1;
    u16 sample_format = 1;
    u16 predictor = 1;
    u32 tile_width = 0;
    u32 tile_length = 0;
    ArenaArray<u64> strip_offsets;
    ArenaArray<u64> strip_byte_counts;
    ArenaArray<u64> color_map;
    ArenaArray<u64> tile_offsets;
    ArenaArray<u64> tile_byte_counts;

    explicit TiffIfdInfo(Arena& arena) noexcept
        : strip_offsets(arena), strip_byte_counts(arena), color_map(arena), tile_offsets(arena),
          tile_byte_counts(arena) {}
};

[[nodiscard]] Result<TiffIfdInfo> parse_ifd(const ByteReader& br, usize ifd_offset, Arena& arena,
                                            u64* next_ifd_offset = nullptr) {
    u64 next_off = 0;
    KEEL_TRY_ASSIGN(const usize n, step_ifd(br, ifd_offset, &next_off));
    if (next_ifd_offset) *next_ifd_offset = next_off;

    TiffIfdInfo img(arena);
    const usize entry_size = br.is_big_tiff ? 20u : 12u;
    const usize voff_off = br.is_big_tiff ? 12u : 8u;
    usize e = ifd_offset + (br.is_big_tiff ? 8u : 2u);

    for (usize i = 0; i < n; ++i, e += entry_size) {
        KEEL_TRY_ASSIGN(const u16 tag, br.u16_at(e + 0));
        KEEL_TRY_ASSIGN(const u16 type, br.u16_at(e + 2));
        u64 count = 0;
        u64 voff = 0;
        if (br.is_big_tiff) {
            KEEL_TRY_ASSIGN(count, br.u64_at(e + 4));
            KEEL_TRY_ASSIGN(voff, br.u64_at(e + voff_off));
        } else {
            KEEL_TRY_ASSIGN(const u32 c32, br.u32_at(e + 4));
            KEEL_TRY_ASSIGN(const u32 v32, br.u32_at(e + voff_off));
            count = c32;
            voff = v32;
        }

        auto first_val = [&]() -> Result<u64> {
            if (count == 0) return u64{0};
            KEEL_TRY_ASSIGN(ArenaArray<u64> vals, read_entry_values(br, type, count, voff, e, arena));
            return vals.empty() ? u64{0} : vals[0];
        };

        switch (tag) {
            case 256: {
                KEEL_TRY_ASSIGN(const u64 v, first_val());
                if (v > 0xFFFFFFFFULL) return ErrorCode::OutOfRange;
                img.width = as<u32>(v);
                break;
            }
            case 257: {
                KEEL_TRY_ASSIGN(const u64 v, first_val());
                if (v > 0xFFFFFFFFULL) return ErrorCode::OutOfRange;
                img.height = as<u32>(v);
                break;
            }
            case 258: {
                if (count > 0) {
                    KEEL_TRY_ASSIGN(ArenaArray<u64> bps_vals, read_entry_values(br, type, count, voff, e, arena));
                    for (usize k = 1; k < bps_vals.size(); ++k) {
                        if (bps_vals[k] != bps_vals[0]) [[unlikely]] {
                            return ErrorCode::NotSupported;
                        }
                    }
                    img.bits_per_sample = as<u16>(bps_vals[0]);
                }
                break;
            }
            case 259: {
                KEEL_TRY_ASSIGN(const u64 v, first_val());
                img.compression = as<u16>(v);
                break;
            }
            case 262: {
                KEEL_TRY_ASSIGN(const u64 v, first_val());
                img.photometric = as<u16>(v);
                break;
            }
            case 273: {
                KEEL_TRY_ASSIGN(img.strip_offsets, read_entry_values(br, type, count, voff, e, arena));
                break;
            }
            case 277: {
                KEEL_TRY_ASSIGN(const u64 v, first_val());
                img.samples_per_pixel = as<u16>(v);
                break;
            }
            case 278: {
                KEEL_TRY_ASSIGN(const u64 v, first_val());
                img.rows_per_strip = (v > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : as<u32>(v);
                break;
            }
            case 279: {
                KEEL_TRY_ASSIGN(img.strip_byte_counts, read_entry_values(br, type, count, voff, e, arena));
                break;
            }
            case 284: {
                KEEL_TRY_ASSIGN(const u64 v, first_val());
                img.planar_config = as<u16>(v);
                break;
            }
            case 317: {
                KEEL_TRY_ASSIGN(const u64 v, first_val());
                img.predictor = as<u16>(v);
                break;
            }
            case 320: {
                KEEL_TRY_ASSIGN(img.color_map, read_entry_values(br, type, count, voff, e, arena));
                break;
            }
            case 322: {
                KEEL_TRY_ASSIGN(const u64 v, first_val());
                if (v > 0xFFFFFFFFULL) return ErrorCode::OutOfRange;
                img.tile_width = as<u32>(v);
                break;
            }
            case 323: {
                KEEL_TRY_ASSIGN(const u64 v, first_val());
                if (v > 0xFFFFFFFFULL) return ErrorCode::OutOfRange;
                img.tile_length = as<u32>(v);
                break;
            }
            case 324: {
                KEEL_TRY_ASSIGN(img.tile_offsets, read_entry_values(br, type, count, voff, e, arena));
                break;
            }
            case 325: {
                KEEL_TRY_ASSIGN(img.tile_byte_counts, read_entry_values(br, type, count, voff, e, arena));
                break;
            }
            case 339: {
                KEEL_TRY_ASSIGN(const u64 v, first_val());
                img.sample_format = as<u16>(v);
                break;
            }
            default:
                break;
        }
    }

    return img;
}

[[nodiscard]] Result<SampleType> resolve_sample_type(u16 sample_format, u16 bits_per_sample) noexcept {
    if (sample_format == 3u) {
        if (bits_per_sample == 32u) return SampleType::F32;
        if (bits_per_sample == 64u) return SampleType::F64;
        return ErrorCode::NotSupported;
    }
    if (sample_format == 2u) {
        if (bits_per_sample == 8u) return SampleType::I8;
        if (bits_per_sample == 16u) return SampleType::I16;
        if (bits_per_sample == 32u) return SampleType::I32;
        return ErrorCode::NotSupported;
    }
    if (sample_format == 1u || sample_format == 0u) {
        if (bits_per_sample == 1u || bits_per_sample == 2u || bits_per_sample == 4u || bits_per_sample == 8u) {
            return SampleType::U8;
        }
        if (bits_per_sample == 16u) return SampleType::U16;
        if (bits_per_sample == 32u) return SampleType::U32;
        return ErrorCode::NotSupported;
    }
    return ErrorCode::NotSupported;
}

[[nodiscard]] Result<Array<u8>> decode_packbits(const u8* src, usize src_len, usize out_hint, usize max_out,
                                                Allocator* alloc) {
    Array<u8> out(alloc);
    out.reserve(out_hint > 0 ? out_hint : (src_len * 2u));
    usize i = 0;
    while (i < src_len) {
        const i8 n = as<i8>(src[i++]);
        if (n == -128) continue;
        if (n >= 0) {
            const usize count = as<usize>(n) + 1u;
            if (count > src_len - i) [[unlikely]] {
                return ErrorCode::EndOfFile;
            }
            if (max_out > 0 && out.size() + count > max_out) [[unlikely]] {
                return ErrorCode::OutOfRange;
            }
            out.append(ArrayRef<u8>(src + i, count));
            i += count;
        } else {
            const usize count = as<usize>(-as<i32>(n)) + 1u;
            if (i >= src_len) [[unlikely]] {
                return ErrorCode::EndOfFile;
            }
            if (max_out > 0 && out.size() + count > max_out) [[unlikely]] {
                return ErrorCode::OutOfRange;
            }
            const u8 val = src[i++];
            const usize prev_sz = out.size();
            out.resize(prev_sz + count, val);
        }
    }
    return out;
}

[[nodiscard]] Result<Array<u8>> decode_lzw(const u8* src, usize src_len, usize out_hint, usize max_out, Arena& arena,
                                           Allocator* alloc) {
    constexpr u32 kClearCode = 256u;
    constexpr u32 kEoiCode = 257u;
    constexpr u32 kNoCode = 0xFFFFFFFFu;
    constexpr u32 kMaxCode = 4096u;

    ArenaTemp tmp = arena.begin_temp();
    u16* prefix = arena.alloc_array<u16>(kMaxCode);
    u8* suffix = arena.alloc_array<u8>(kMaxCode);
    u8* first_char = arena.alloc_array<u8>(kMaxCode);
    u8* stack = arena.alloc_array<u8>(kMaxCode);

    for (u32 c = 0; c < 256u; ++c) {
        prefix[c] = 0xFFFFu;
        suffix[c] = as<u8>(c);
        first_char[c] = as<u8>(c);
    }

    u32 next_code = 258u;
    i32 code_width = 9;
    u32 prev_code = kNoCode;

    Array<u8> out(alloc);
    out.reserve(out_hint > 0 ? out_hint : (src_len * 2u));

    const usize total_bits = src_len * 8u;
    usize bit_pos = 0;

    while (bit_pos + as<usize>(code_width) <= total_bits) {
        u32 code = 0;
        for (i32 i = 0; i < code_width; ++i) {
            const usize byte_pos = bit_pos >> 3;
            const i32 bit_off = 7 - as<i32>(bit_pos & 7u);
            code = (code << 1) | ((src[byte_pos] >> bit_off) & 1u);
            ++bit_pos;
        }

        if (code == kEoiCode) {
            break;
        }
        if (code == kClearCode) {
            next_code = 258u;
            code_width = 9;
            prev_code = kNoCode;
            continue;
        }

        usize sp = 0;
        u8 entry_first = 0;

        if (code < next_code) {
            u32 cur = code;
            while (cur >= 258u) {
                if (sp >= kMaxCode) [[unlikely]] {
                    return ErrorCode::InvalidData;
                }
                stack[sp++] = suffix[cur];
                cur = prefix[cur];
            }
            if (cur >= 256u) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            entry_first = as<u8>(cur);
            stack[sp++] = entry_first;
        } else if (code == next_code && prev_code != kNoCode) {
            entry_first = first_char[prev_code];
            stack[sp++] = entry_first;
            u32 cur = prev_code;
            while (cur >= 258u) {
                if (sp >= kMaxCode) [[unlikely]] {
                    return ErrorCode::InvalidData;
                }
                stack[sp++] = suffix[cur];
                cur = prefix[cur];
            }
            if (cur >= 256u) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            stack[sp++] = as<u8>(cur);
        } else {
            return ErrorCode::InvalidData;
        }

        if (max_out > 0 && out.size() + sp > max_out) [[unlikely]] {
            return ErrorCode::OutOfRange;
        }
        for (usize k = sp; k > 0; --k) {
            out.push_back(stack[k - 1]);
        }

        if (prev_code != kNoCode && next_code < kMaxCode) {
            prefix[next_code] = as<u16>(prev_code);
            suffix[next_code] = entry_first;
            first_char[next_code] = first_char[prev_code];
            ++next_code;
            if (next_code + 1u == (1u << code_width) && code_width < 12) {
                ++code_width;
            }
        }
        prev_code = code;
    }

    return out;
}

[[nodiscard]] Result<Array<u8>> decompress_block(const ByteReader& br, u64 off64, u64 cnt64, u16 compression,
                                                 usize hint_bytes, usize max_out, Arena& arena, Allocator* alloc) {
    if (off64 > as<u64>(br.size) || cnt64 > as<u64>(br.size) - off64) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    const usize off = as<usize>(off64);
    const usize cnt = as<usize>(cnt64);
    const u8* src = br.buf + off;

    switch (compression) {
        case 1u: {
            if (max_out > 0 && cnt > max_out) [[unlikely]] {
                return ErrorCode::OutOfRange;
            }
            Array<u8> out(alloc);
            out.append(ArrayRef<u8>(src, cnt));
            return out;
        }
        case 32773u:
            return decode_packbits(src, cnt, hint_bytes, max_out, alloc);
        case 5u:
            return decode_lzw(src, cnt, hint_bytes, max_out, arena, alloc);
        case 8u:
        case 32946u: {
            const keel::compress::InflateOptions inf_opts{
                .expected_size_hint = hint_bytes,
                .max_output_bytes = max_out,
                .allocator = alloc,
            };
            auto res = keel::compress::zlib_decompress(ArrayRef<u8>(src, cnt), inf_opts);
            if (res.is_ok() || res.error().code == ErrorCode::OutOfRange) {
                return res;
            }
            // Fallback for rare raw-Deflate TIFF strips without RFC 1950 wrapper.
            return keel::compress::inflate_raw(ArrayRef<u8>(src, cnt), inf_opts);
        }
        default:
            return ErrorCode::NotSupported;
    }
}

void apply_horizontal_undiff(u8* buf, usize height, usize width, usize spp, usize bps, bool big_endian) noexcept {
    const usize row_bytes = width * spp * bps;
    if (bps == 1) {
        for (usize r = 0; r < height; ++r) {
            u8* row = buf + r * row_bytes;
            for (usize c = 1; c < width; ++c) {
                for (usize s = 0; s < spp; ++s) {
                    row[c * spp + s] = as<u8>(row[c * spp + s] + row[(c - 1) * spp + s]);
                }
            }
        }
    } else if (bps == 2) {
        for (usize r = 0; r < height; ++r) {
            u8* row = buf + r * row_bytes;
            for (usize c = 1; c < width; ++c) {
                for (usize s = 0; s < spp; ++s) {
                    const usize off = (c * spp + s) * 2u;
                    const usize pof = ((c - 1) * spp + s) * 2u;
                    const u16 cur = big_endian ? detail::read_u16_be(row + off) : detail::read_u16_le(row + off);
                    const u16 prv = big_endian ? detail::read_u16_be(row + pof) : detail::read_u16_le(row + pof);
                    const u16 v = as<u16>(cur + prv);
                    if (big_endian) {
                        row[off] = as<u8>((v >> 8) & 0xFFu);
                        row[off + 1] = as<u8>(v & 0xFFu);
                    } else {
                        row[off] = as<u8>(v & 0xFFu);
                        row[off + 1] = as<u8>((v >> 8) & 0xFFu);
                    }
                }
            }
        }
    } else if (bps == 4) {
        for (usize r = 0; r < height; ++r) {
            u8* row = buf + r * row_bytes;
            for (usize c = 1; c < width; ++c) {
                for (usize s = 0; s < spp; ++s) {
                    const usize off = (c * spp + s) * 4u;
                    const usize pof = ((c - 1) * spp + s) * 4u;
                    const u32 cur = big_endian ? detail::read_u32_be(row + off) : detail::read_u32_le(row + off);
                    const u32 prv = big_endian ? detail::read_u32_be(row + pof) : detail::read_u32_le(row + pof);
                    const u32 v = cur + prv;
                    if (big_endian) {
                        row[off + 0] = as<u8>((v >> 24) & 0xFFu);
                        row[off + 1] = as<u8>((v >> 16) & 0xFFu);
                        row[off + 2] = as<u8>((v >> 8) & 0xFFu);
                        row[off + 3] = as<u8>(v & 0xFFu);
                    } else {
                        row[off + 0] = as<u8>(v & 0xFFu);
                        row[off + 1] = as<u8>((v >> 8) & 0xFFu);
                        row[off + 2] = as<u8>((v >> 16) & 0xFFu);
                        row[off + 3] = as<u8>((v >> 24) & 0xFFu);
                    }
                }
            }
        }
    }
}

void apply_float_predictor_undiff(u8* buf, usize height, usize width, usize spp, usize bps, u8* tmp_row) noexcept {
    // Adobe TIFF Technical Note 3: per row, undo horizontal byte differencing with stride `spp`,
    // then re-interleave MSB-first byte planes into native-endian IEEE 754 floating-point samples.
    const usize num_samples = width * spp;
    const usize row_bytes = num_samples * bps;
    const bool host_be = detail::is_host_big_endian();

    for (usize r = 0; r < height; ++r) {
        u8* row = buf + r * row_bytes;
        for (usize i = spp; i < row_bytes; ++i) {
            row[i] = as<u8>(row[i] + row[i - spp]);
        }
        mem_copy(tmp_row, row, row_bytes);
        if (host_be) {
            for (usize s = 0; s < num_samples; ++s) {
                for (usize b = 0; b < bps; ++b) {
                    row[s * bps + b] = tmp_row[b * num_samples + s];
                }
            }
        } else {
            for (usize s = 0; s < num_samples; ++s) {
                for (usize b = 0; b < bps; ++b) {
                    row[s * bps + (bps - 1u - b)] = tmp_row[b * num_samples + s];
                }
            }
        }
    }
}

void unpack_subbyte_row(const u8* src, u8* dst, usize width, u16 bits_per_sample) noexcept {
    const u8 mask = as<u8>((1u << bits_per_sample) - 1u);
    for (usize x = 0; x < width; ++x) {
        const usize bit_idx = x * bits_per_sample;
        const usize byte_idx = bit_idx >> 3;
        const i32 shift = as<i32>(8u - bits_per_sample - (bit_idx & 7u));
        dst[x] = as<u8>((src[byte_idx] >> shift) & mask);
    }
}

// ============================================================================
// Encoder Helpers (PackBits, LZW, Predictor, Classic/BigTIFF IFD Writer)
// ============================================================================

void write_u16_le_at(Array<u8>& buf, usize off, u16 v) noexcept {
    buf[off + 0] = as<u8>(v & 0xFFu);
    buf[off + 1] = as<u8>((v >> 8) & 0xFFu);
}

void write_u32_le_at(Array<u8>& buf, usize off, u32 v) noexcept {
    buf[off + 0] = as<u8>(v & 0xFFu);
    buf[off + 1] = as<u8>((v >> 8) & 0xFFu);
    buf[off + 2] = as<u8>((v >> 16) & 0xFFu);
    buf[off + 3] = as<u8>((v >> 24) & 0xFFu);
}

void write_u64_le_at(Array<u8>& buf, usize off, u64 v) noexcept {
    write_u32_le_at(buf, off + 0, as<u32>(v & 0xFFFFFFFFULL));
    write_u32_le_at(buf, off + 4, as<u32>((v >> 32) & 0xFFFFFFFFULL));
}

[[nodiscard]] Array<u8> encode_packbits(const u8* src, usize n, Allocator* alloc) {
    Array<u8> out(alloc);
    out.reserve(n + (n / 64u) + 8u);
    usize i = 0;
    while (i < n) {
        usize run_end = i + 1u;
        while (run_end < n && src[run_end] == src[i] && run_end - i < 128u) {
            ++run_end;
        }
        const usize run_len = run_end - i;
        if (run_len >= 3u) {
            out.push_back(as<u8>(as<i8>(-(as<i32>(run_len) - 1))));
            out.push_back(src[i]);
            i = run_end;
            continue;
        }
        usize lit_end = i + 1u;
        while (lit_end < n && lit_end - i < 128u) {
            if (lit_end + 2u < n && src[lit_end] == src[lit_end + 1u] && src[lit_end + 1u] == src[lit_end + 2u]) {
                break;
            }
            ++lit_end;
        }
        const usize lit_len = lit_end - i;
        out.push_back(as<u8>(lit_len - 1u));
        out.append(ArrayRef<u8>(src + i, lit_len));
        i = lit_end;
    }
    return out;
}

[[nodiscard]] Array<u8> encode_lzw(const u8* src, usize n, Arena& arena, Allocator* alloc) {
    constexpr u32 kClearCode = 256u;
    constexpr u32 kEoiCode = 257u;
    // Per TIFF 6.0 Section 13, emit ClearCode before code 4094 for maximum decoder compatibility.
    constexpr u32 kMaxEntries = 4094u;
    constexpr u32 kHashSize = 8192u;
    constexpr u32 kHashMask = kHashSize - 1u;
    constexpr u32 kEmptyKey = 0xFFFFFFFFu;

    ArenaTemp tmp = arena.begin_temp();
    u32* slot_key = arena.alloc_array<u32>(kHashSize);
    u16* slot_val = arena.alloc_array<u16>(kHashSize);
    mem_set(slot_key, 0xFF, kHashSize * sizeof(u32));

    Array<u8> out(alloc);
    out.reserve((n / 2u) + 64u);

    u32 bit_buf = 0;
    i32 bit_count = 0;
    i32 code_width = 9;
    u32 next_code = 258u;

    auto write_bits = [&](u32 code) {
        bit_buf = (bit_buf << code_width) | (code & ((1u << code_width) - 1u));
        bit_count += code_width;
        while (bit_count >= 8) {
            bit_count -= 8;
            out.push_back(as<u8>((bit_buf >> bit_count) & 0xFFu));
        }
    };

    auto flush_bits = [&]() {
        if (bit_count > 0) {
            out.push_back(as<u8>((bit_buf << (8 - bit_count)) & 0xFFu));
            bit_count = 0;
            bit_buf = 0;
        }
    };

    write_bits(kClearCode);
    if (n == 0) {
        write_bits(kEoiCode);
        flush_bits();
        return out;
    }

    u32 prefix = src[0];
    for (usize i = 1; i < n; ++i) {
        const u8 c = src[i];
        const u32 key = (prefix << 8) | as<u32>(c);
        u32 h = ((key * 2654435761u) >> 19) & kHashMask;
        bool found = false;
        while (slot_key[h] != kEmptyKey) {
            if (slot_key[h] == key) {
                prefix = slot_val[h];
                found = true;
                break;
            }
            h = (h + 1u) & kHashMask;
        }

        if (!found) {
            write_bits(prefix);
            if (next_code < kMaxEntries) {
                slot_key[h] = key;
                slot_val[h] = as<u16>(next_code++);
                if (next_code == (1u << code_width) && code_width < 12) {
                    ++code_width;
                }
            } else {
                write_bits(kClearCode);
                mem_set(slot_key, 0xFF, kHashSize * sizeof(u32));
                next_code = 258u;
                code_width = 9;
            }
            prefix = c;
        }
    }

    write_bits(prefix);
    write_bits(kEoiCode);
    flush_bits();
    return out;
}

void apply_horizontal_diff_le(u8* buf, usize height, usize width, usize spp, usize bps) noexcept {
    const usize row_bytes = width * spp * bps;
    if (bps == 1) {
        for (usize r = 0; r < height; ++r) {
            u8* row = buf + r * row_bytes;
            for (usize c = width; c-- > 1u;) {
                for (usize s = 0; s < spp; ++s) {
                    row[c * spp + s] = as<u8>(row[c * spp + s] - row[(c - 1u) * spp + s]);
                }
            }
        }
    } else if (bps == 2) {
        for (usize r = 0; r < height; ++r) {
            u8* row = buf + r * row_bytes;
            for (usize c = width; c-- > 1u;) {
                for (usize s = 0; s < spp; ++s) {
                    const usize off = (c * spp + s) * 2u;
                    const usize pof = ((c - 1u) * spp + s) * 2u;
                    const u16 cur = detail::read_u16_le(row + off);
                    const u16 prv = detail::read_u16_le(row + pof);
                    const u16 v = as<u16>(cur - prv);
                    row[off + 0] = as<u8>(v & 0xFFu);
                    row[off + 1] = as<u8>((v >> 8) & 0xFFu);
                }
            }
        }
    } else if (bps == 4) {
        for (usize r = 0; r < height; ++r) {
            u8* row = buf + r * row_bytes;
            for (usize c = width; c-- > 1u;) {
                for (usize s = 0; s < spp; ++s) {
                    const usize off = (c * spp + s) * 4u;
                    const usize pof = ((c - 1u) * spp + s) * 4u;
                    const u32 cur = detail::read_u32_le(row + off);
                    const u32 prv = detail::read_u32_le(row + pof);
                    const u32 v = cur - prv;
                    row[off + 0] = as<u8>(v & 0xFFu);
                    row[off + 1] = as<u8>((v >> 8) & 0xFFu);
                    row[off + 2] = as<u8>((v >> 16) & 0xFFu);
                    row[off + 3] = as<u8>((v >> 24) & 0xFFu);
                }
            }
        }
    }
}

struct SampleEncodingSpec {
    usize bytes_per_sample = 1;
    u16 bits_per_sample = 8;
    u16 sample_format = 1; // 1 = uint, 2 = int, 3 = IEEE float
};

[[nodiscard]] Result<SampleEncodingSpec> map_sample_encoding(SampleType st) noexcept {
    switch (st) {
        case SampleType::U8:
            return SampleEncodingSpec{1, 8, 1};
        case SampleType::U16:
            return SampleEncodingSpec{2, 16, 1};
        case SampleType::U32:
            return SampleEncodingSpec{4, 32, 1};
        case SampleType::I8:
            return SampleEncodingSpec{1, 8, 2};
        case SampleType::I16:
            return SampleEncodingSpec{2, 16, 2};
        case SampleType::I32:
            return SampleEncodingSpec{4, 32, 2};
        case SampleType::F32:
            return SampleEncodingSpec{4, 32, 3};
        case SampleType::F64:
            return SampleEncodingSpec{8, 64, 3};
    }
    return ErrorCode::InvalidArgument;
}

[[nodiscard]] Result<u16> map_tiff_compression(TiffCompression comp) noexcept {
    switch (comp) {
        case TiffCompression::None:
            return u16{1};
        case TiffCompression::PackBits:
            return u16{32773};
        case TiffCompression::Lzw:
            return u16{5};
        case TiffCompression::Deflate:
            return u16{8};
    }
    return ErrorCode::InvalidArgument;
}

struct RawIfdEntry {
    u16 tag = 0;
    u16 type = 0;
    u64 count = 0;
    u8 inline_bytes[8]{};
    u64 external_offset = 0;
    bool is_external = false;
};

[[nodiscard]] RawIfdEntry make_u16_entry(u16 tag, u16 val) noexcept {
    RawIfdEntry e{.tag = tag, .type = 3, .count = 1};
    e.inline_bytes[0] = as<u8>(val & 0xFFu);
    e.inline_bytes[1] = as<u8>((val >> 8) & 0xFFu);
    return e;
}

[[nodiscard]] RawIfdEntry make_u32_entry(u16 tag, u32 val) noexcept {
    RawIfdEntry e{.tag = tag, .type = 4, .count = 1};
    e.inline_bytes[0] = as<u8>(val & 0xFFu);
    e.inline_bytes[1] = as<u8>((val >> 8) & 0xFFu);
    e.inline_bytes[2] = as<u8>((val >> 16) & 0xFFu);
    e.inline_bytes[3] = as<u8>((val >> 24) & 0xFFu);
    return e;
}

[[nodiscard]] RawIfdEntry make_offset_or_size_entry(u16 tag, u64 val, bool big_tiff) noexcept {
    if (!big_tiff) {
        return make_u32_entry(tag, as<u32>(val));
    }
    RawIfdEntry e{.tag = tag, .type = 16, .count = 1};
    for (i32 i = 0; i < 8; ++i) {
        e.inline_bytes[i] = as<u8>((val >> (8 * i)) & 0xFFu);
    }
    return e;
}

void emit_ifd(Array<u8>& buf, ArrayRef<RawIfdEntry> entries, bool big_tiff, usize prev_next_slot) {
    if ((buf.size() & 1u) != 0u) {
        buf.push_back(0u);
    }
    const usize ifd_off = buf.size();
    const usize hdr_sz = big_tiff ? 8u : 2u;
    const usize entry_sz = big_tiff ? 20u : 12u;
    const usize next_sz = big_tiff ? 8u : 4u;
    const usize ifd_bytes = hdr_sz + entries.size() * entry_sz + next_sz;

    buf.resize(ifd_off + ifd_bytes, u8{0});

    if (big_tiff) {
        write_u64_le_at(buf, ifd_off, as<u64>(entries.size()));
    } else {
        write_u16_le_at(buf, ifd_off, as<u16>(entries.size()));
    }

    for (usize i = 0; i < entries.size(); ++i) {
        const RawIfdEntry& en = entries[i];
        const usize pos = ifd_off + hdr_sz + i * entry_sz;
        write_u16_le_at(buf, pos + 0, en.tag);
        write_u16_le_at(buf, pos + 2, en.type);
        if (big_tiff) {
            write_u64_le_at(buf, pos + 4, en.count);
            if (en.is_external) {
                write_u64_le_at(buf, pos + 12, en.external_offset);
            } else {
                mem_copy(buf.data() + pos + 12, en.inline_bytes, 8);
            }
        } else {
            write_u32_le_at(buf, pos + 4, as<u32>(en.count));
            if (en.is_external) {
                write_u32_le_at(buf, pos + 8, as<u32>(en.external_offset));
            } else {
                mem_copy(buf.data() + pos + 8, en.inline_bytes, 4);
            }
        }
    }

    if (big_tiff) {
        write_u64_le_at(buf, prev_next_slot, as<u64>(ifd_off));
    } else {
        write_u32_le_at(buf, prev_next_slot, as<u32>(ifd_off));
    }
}

// Upgrades an existing Little-Endian Classic TIFF container into a Little-Endian BigTIFF container
// in `out_buf`, returning the 8-byte `next_ifd` slot offset of the last migrated IFD.
[[nodiscard]] Result<usize> upgrade_classic_le_to_bigtiff(const ByteReader& src_br, Array<u8>& out_buf, Arena& arena) {
    // Emit 16-byte BigTIFF header + original payload bytes [8 .. src_br.size), shifting all payload offsets by +8.
    constexpr u64 kDelta = 8u;
    out_buf.resize(16u + (src_br.size - 8u), u8{0});
    out_buf[0] = 'I';
    out_buf[1] = 'I';
    write_u16_le_at(out_buf, 2, 43u);
    write_u16_le_at(out_buf, 4, 8u);
    write_u16_le_at(out_buf, 6, 0u);
    write_u64_le_at(out_buf, 8, 0u);
    if (src_br.size > 8u) {
        mem_copy(out_buf.data() + 16u, src_br.buf + 8u, src_br.size - 8u);
    }

    usize prev_next_slot = 8u;
    KEEL_TRY_ASSIGN(u64 off, first_ifd_offset(src_br));
    u32 pages = 0;

    while (off != 0) {
        if (++pages > kMaxIfds) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        u64 next_off = 0;
        KEEL_TRY_ASSIGN(const usize n_entries, step_ifd(src_br, off, &next_off));
        const usize ifd_pos = as<usize>(off);

        ArenaTemp page_tmp = arena.begin_temp();
        ArenaArray<RawIfdEntry> new_entries(arena, n_entries);

        for (usize i = 0; i < n_entries; ++i) {
            const usize e = ifd_pos + 2u + i * 12u;
            KEEL_TRY_ASSIGN(const u16 tag, src_br.u16_at(e + 0));
            KEEL_TRY_ASSIGN(const u16 type, src_br.u16_at(e + 2));
            KEEL_TRY_ASSIGN(const u32 count32, src_br.u32_at(e + 4));
            KEEL_TRY_ASSIGN(const u32 voff32, src_br.u32_at(e + 8));

            if (type == 0 || type > 18 || kTypeWidth[type] == 0) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            const usize w = kTypeWidth[type];
            const u64 count = count32;
            if (count > kMaxTagArrayCount) [[unlikely]] {
                return ErrorCode::OutOfRange;
            }
            const usize total = as<usize>(count) * w;

            // StripOffsets (273) and TileOffsets (324) reference shifted file offsets (+8).
            if (tag == 273u || tag == 324u) {
                KEEL_TRY_ASSIGN(ArenaArray<u64> offs, read_entry_values(src_br, type, count, voff32, e, arena));
                RawIfdEntry ne{.tag = tag, .type = 16, .count = count};
                if (offs.size() == 1) {
                    const u64 shifted = offs[0] + kDelta;
                    for (i32 b = 0; b < 8; ++b) {
                        ne.inline_bytes[b] = as<u8>((shifted >> (8 * b)) & 0xFFu);
                    }
                } else if (offs.size() > 1) {
                    if ((out_buf.size() & 1u) != 0u) out_buf.push_back(0u);
                    ne.is_external = true;
                    ne.external_offset = out_buf.size();
                    for (usize k = 0; k < offs.size(); ++k) {
                        detail::push_u64_le(out_buf, offs[k] + kDelta);
                    }
                }
                new_entries.push_back(ne);
                continue;
            }

            RawIfdEntry ne{.tag = tag, .type = type, .count = count};
            const usize src_val_pos = (total <= 4u) ? (e + 8u) : as<usize>(voff32);
            KEEL_TRY(src_br.check(src_val_pos, total));
            if (total <= 8u) {
                if (total > 0) {
                    mem_copy(ne.inline_bytes, src_br.buf + src_val_pos, total);
                }
            } else {
                ne.is_external = true;
                ne.external_offset = as<u64>(voff32) + kDelta;
            }
            new_entries.push_back(ne);
        }

        emit_ifd(out_buf, new_entries.as_ref(), /*big_tiff=*/true, prev_next_slot);
        prev_next_slot = out_buf.size() - 8u;
        off = next_off;
    }

    return prev_next_slot;
}

} // namespace tiff_impl
} // namespace

Result<u32> tiff_page_count(ArrayRef<u8> bytes) {
    KEEL_TRY_ASSIGN(const tiff_impl::ByteReader br, tiff_impl::open_tiff(bytes));
    KEEL_TRY_ASSIGN(u64 off, tiff_impl::first_ifd_offset(br));
    if (off == 0) [[unlikely]] {
        return ErrorCode::InvalidData;
    }

    u32 count = 0;
    while (off != 0) {
        if (++count > tiff_impl::kMaxIfds) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        u64 next_off = 0;
        KEEL_TRY(tiff_impl::step_ifd(br, off, &next_off));
        off = next_off;
    }
    return count;
}

Result<ImageInfo> peek_tiff(ArrayRef<u8> bytes) {
    KEEL_TRY_ASSIGN(const tiff_impl::ByteReader br, tiff_impl::open_tiff(bytes));
    KEEL_TRY_ASSIGN(const u32 pages, tiff_page_count(bytes));
    KEEL_TRY_ASSIGN(const usize ifd_off, tiff_impl::locate_ifd_for_page(br, 0u));

    ArenaTemp scratch = get_scratch();
    KEEL_TRY_ASSIGN(const tiff_impl::TiffIfdInfo timg, tiff_impl::parse_ifd(br, ifd_off, scratch.arena()));

    if (timg.width == 0 || timg.height == 0 || timg.samples_per_pixel == 0 || timg.bits_per_sample == 0) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    KEEL_TRY_ASSIGN(const SampleType st, tiff_impl::resolve_sample_type(timg.sample_format, timg.bits_per_sample));

    return ImageInfo{
        .format = ImageFormat::Tiff,
        .width = timg.width,
        .height = timg.height,
        .channels = timg.samples_per_pixel,
        .bits_per_sample = timg.bits_per_sample,
        .sample_type = st,
        .has_palette = (timg.photometric == 3u),
        .page_count = pages,
    };
}

Result<Image> decode_tiff(ArrayRef<u8> bytes, const ImageDecodeOptions& opts) {
    KEEL_TRY_ASSIGN(const tiff_impl::ByteReader br, tiff_impl::open_tiff(bytes));
    KEEL_TRY_ASSIGN(const usize ifd_off, tiff_impl::locate_ifd_for_page(br, opts.page));

    ArenaTemp scratch = get_scratch(opts.allocator);
    KEEL_TRY_ASSIGN(const tiff_impl::TiffIfdInfo timg, tiff_impl::parse_ifd(br, ifd_off, scratch.arena()));

    if (timg.width == 0 || timg.height == 0 || timg.samples_per_pixel == 0) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (timg.photometric > 3u) [[unlikely]] {
        return ErrorCode::NotSupported;
    }
    if (timg.samples_per_pixel > 4u) [[unlikely]] {
        return ErrorCode::NotSupported;
    }
    if (timg.planar_config != 1u && timg.planar_config != 2u) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (timg.predictor != 1u && timg.predictor != 2u && timg.predictor != 3u) [[unlikely]] {
        return ErrorCode::NotSupported;
    }

    KEEL_TRY_ASSIGN(const SampleType sample_type,
                    tiff_impl::resolve_sample_type(timg.sample_format, timg.bits_per_sample));
    if (timg.predictor == 3u && sample_type != SampleType::F32 && sample_type != SampleType::F64) [[unlikely]] {
        return ErrorCode::NotSupported;
    }
    const bool subbyte = (timg.bits_per_sample < 8u);
    if (subbyte && (timg.samples_per_pixel != 1u || timg.predictor != 1u)) [[unlikely]] {
        return ErrorCode::NotSupported;
    }

    KEEL_TRY_ASSIGN(const usize total_bytes,
                    detail::validate_decode_allocation(timg.width, timg.height, timg.samples_per_pixel, sample_type,
                                                       opts));
    if (opts.expand_indexed && timg.photometric == 3u) {
        KEEL_TRY(detail::validate_decode_allocation(timg.width, timg.height, 3u, SampleType::U16, opts));
    }

    const usize H = timg.height;
    const usize W = timg.width;
    const usize S = timg.samples_per_pixel;
    const usize bps = sample_byte_size(sample_type);
    const usize dst_row_bytes = W * S * bps;

    const bool will_expand = (opts.expand_indexed && timg.photometric == 3u);
    Allocator* work_alloc = will_expand ? scratch.allocator() : opts.allocator;

    Image img;
    img.width = timg.width;
    img.height = timg.height;
    img.channels = timg.samples_per_pixel;
    img.sample_type = sample_type;
    img.palette_bits = 0;
    img.pixels = Array<u8>(total_bytes, u8{0}, work_alloc);
    img.palette = Array<u16>(work_alloc);

    if (timg.photometric == 3u) {
        if (timg.samples_per_pixel != 1u || timg.bits_per_sample > 16u) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        const usize k_entries = usize{1} << timg.bits_per_sample;
        if (timg.color_map.size() != 3u * k_entries) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        img.palette = Array<u16>(3u * k_entries, u16{0}, work_alloc);
        img.palette_bits = 16u;
        u16* pal = img.palette.data();
        for (usize k = 0; k < k_entries; ++k) {
            pal[k * 3u + 0u] = as<u16>(timg.color_map[0u * k_entries + k] & 0xFFFFu);
            pal[k * 3u + 1u] = as<u16>(timg.color_map[1u * k_entries + k] & 0xFFFFu);
            pal[k * 3u + 2u] = as<u16>(timg.color_map[2u * k_entries + k] & 0xFFFFu);
        }
    }

    u8* dst = img.pixels.data();
    const bool tiled = (timg.tile_width > 0u && timg.tile_length > 0u);
    const bool planar = (timg.planar_config == 2u);
    const usize num_planes = planar ? S : 1u;
    const usize spp_block = planar ? 1u : S;

    auto put_chunky = [&](const u8* block, usize r0, usize c0, usize rh, usize cw, usize src_stride) {
        for (usize r = 0; r < rh; ++r) {
            const usize out_r = r0 + r;
            if (out_r >= H) break;
            const usize copy_cols = (cw < W - c0) ? cw : (W - c0);
            const u8* s_row = block + r * src_stride;
            u8* d_row = dst + out_r * dst_row_bytes + c0 * S * bps;
            if (subbyte) {
                tiff_impl::unpack_subbyte_row(s_row, d_row, copy_cols, timg.bits_per_sample);
            } else {
                mem_copy(d_row, s_row, copy_cols * S * bps);
            }
        }
    };

    auto put_planar = [&](const u8* block, usize r0, usize c0, usize rh, usize cw, usize plane_idx, usize src_stride) {
        for (usize r = 0; r < rh; ++r) {
            const usize out_r = r0 + r;
            if (out_r >= H) break;
            const usize copy_cols = (cw < W - c0) ? cw : (W - c0);
            const u8* s_row = block + r * src_stride;
            u8* d_row = dst + out_r * dst_row_bytes + c0 * S * bps + plane_idx * bps;
            for (usize c = 0; c < copy_cols; ++c) {
                mem_copy(d_row + c * S * bps, s_row + c * bps, bps);
            }
        }
    };

    // Use default heap allocator for temporary decompressed blocks when large so the 64 MiB arena stays free.
    const usize block_max_out = (opts.max_bytes > 0) ? (opts.max_bytes * 2u + 65536u) : keel::compress::kDefaultMaxDecompressBytes;

    if (tiled) {
        const usize tw = timg.tile_width;
        const usize tl = timg.tile_length;
        const usize tiles_across = (W + tw - 1u) / tw;
        const usize tiles_down = (H + tl - 1u) / tl;
        if (tiles_across > Limits<usize>::kMax / tiles_down) [[unlikely]] {
            return ErrorCode::OutOfRange;
        }
        const usize tiles_per_plane = tiles_across * tiles_down;
        if (timg.tile_offsets.size() != tiles_per_plane * num_planes ||
            timg.tile_byte_counts.size() != tiles_per_plane * num_planes) [[unlikely]] {
            return ErrorCode::InvalidData;
        }

        if (!subbyte && tw > Limits<usize>::kMax / (spp_block * bps)) [[unlikely]] {
            return ErrorCode::OutOfRange;
        }
        const usize src_stride =
            subbyte ? ((tw * timg.bits_per_sample + 7u) / 8u) : (tw * spp_block * bps);
        if (src_stride > 0 && tl > Limits<usize>::kMax / src_stride) [[unlikely]] {
            return ErrorCode::OutOfRange;
        }
        const usize tile_bytes = src_stride * tl;
        if (tile_bytes > block_max_out) [[unlikely]] {
            return ErrorCode::OutOfRange;
        }

        for (usize p = 0; p < num_planes; ++p) {
            for (usize ty = 0; ty < tiles_down; ++ty) {
                for (usize tx = 0; tx < tiles_across; ++tx) {
                    const usize idx = p * tiles_per_plane + ty * tiles_across + tx;
                    ArenaTemp tile_tmp = scratch.arena().begin_temp();
                    Allocator* blk_alloc = (tile_bytes <= 4ULL * 1024 * 1024) ? tile_tmp.allocator() : nullptr;
                    KEEL_TRY_ASSIGN(Array<u8> tile,
                                    tiff_impl::decompress_block(br, timg.tile_offsets[idx], timg.tile_byte_counts[idx],
                                                                timg.compression, tile_bytes, block_max_out,
                                                                scratch.arena(), blk_alloc));
                    if (tile.size() < tile_bytes) {
                        tile.resize(tile_bytes, u8{0});
                    }
                    if (timg.predictor == 2u) {
                        tiff_impl::apply_horizontal_undiff(tile.data(), tl, tw, spp_block, bps, br.big_endian);
                    } else if (timg.predictor == 3u) {
                        Array<u8> tmp_row(src_stride, u8{0}, tile_tmp.allocator());
                        tiff_impl::apply_float_predictor_undiff(tile.data(), tl, tw, spp_block, bps, tmp_row.data());
                    }
                    const usize r0 = ty * tl;
                    const usize c0 = tx * tw;
                    if (planar) {
                        put_planar(tile.data(), r0, c0, tl, tw, p, src_stride);
                    } else {
                        put_chunky(tile.data(), r0, c0, tl, tw, src_stride);
                    }
                }
            }
        }
    } else {
        if (timg.strip_offsets.empty() || timg.strip_offsets.size() != timg.strip_byte_counts.size()) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        if (timg.strip_offsets.size() % num_planes != 0u) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        const usize strips_per_plane = timg.strip_offsets.size() / num_planes;
        const usize rps_eff = (timg.rows_per_strip > 0u && timg.rows_per_strip < 0x80000000u)
                                  ? as<usize>(timg.rows_per_strip)
                                  : H;
        const usize src_stride =
            subbyte ? ((W * timg.bits_per_sample + 7u) / 8u) : (W * spp_block * bps);

        for (usize p = 0; p < num_planes; ++p) {
            for (usize si = 0; si < strips_per_plane; ++si) {
                if (si > (H - 1u) / rps_eff) break;
                const usize r0 = si * rps_eff;
                const usize rh = (rps_eff < H - r0) ? rps_eff : (H - r0);
                const usize hint = src_stride * rh;
                const usize global_idx = p * strips_per_plane + si;

                ArenaTemp strip_tmp = scratch.arena().begin_temp();
                Allocator* blk_alloc = (hint <= 4ULL * 1024 * 1024) ? strip_tmp.allocator() : nullptr;
                KEEL_TRY_ASSIGN(Array<u8> strip,
                                tiff_impl::decompress_block(br, timg.strip_offsets[global_idx],
                                                            timg.strip_byte_counts[global_idx], timg.compression, hint,
                                                            block_max_out, scratch.arena(), blk_alloc));
                if (strip.size() < hint) {
                    strip.resize(hint, u8{0});
                }
                if (timg.predictor == 2u) {
                    tiff_impl::apply_horizontal_undiff(strip.data(), rh, W, spp_block, bps, br.big_endian);
                } else if (timg.predictor == 3u) {
                    Array<u8> tmp_row(src_stride, u8{0}, strip_tmp.allocator());
                    tiff_impl::apply_float_predictor_undiff(strip.data(), rh, W, spp_block, bps, tmp_row.data());
                }
                if (planar) {
                    put_planar(strip.data(), r0, 0u, rh, W, p, src_stride);
                } else {
                    put_chunky(strip.data(), r0, 0u, rh, W, src_stride);
                }
            }
        }
    }

    // Convert multi-byte samples from TIFF endianness to native host endianness if needed.
    // Predictor == 3 outputs directly in native host endianness from MSB-first byte planes.
    if (bps > 1u && timg.predictor != 3u && br.big_endian != detail::is_host_big_endian()) {
        const usize total_samples = H * W * S;
        if (bps == 2u) {
            for (usize i = 0; i < total_samples; ++i) {
                const u16 swapped = byteswap(detail::read_u16_native(dst + i * 2u));
                detail::write_u16_native(dst + i * 2u, swapped);
            }
        } else if (bps == 4u) {
            for (usize i = 0; i < total_samples; ++i) {
                const u32 swapped = byteswap(detail::read_u32_native(dst + i * 4u));
                detail::write_u32_native(dst + i * 4u, swapped);
            }
        } else if (bps == 8u) {
            for (usize i = 0; i < total_samples; ++i) {
                const u64 swapped = byteswap(detail::read_u64_native(dst + i * 8u));
                detail::write_u64_native(dst + i * 8u, swapped);
            }
        }
    }

    // Handle Photometric == 0 (WhiteIsZero) sample inversion.
    if (timg.photometric == 0u) {
        const usize total_samples = H * W * S;
        if (sample_type == SampleType::U8) {
            const u8 max_val = subbyte ? as<u8>((1u << timg.bits_per_sample) - 1u) : u8{255};
            for (usize i = 0; i < total_samples; ++i) {
                dst[i] = as<u8>(max_val - dst[i]);
            }
        } else if (sample_type == SampleType::U16) {
            for (usize i = 0; i < total_samples; ++i) {
                const u16 v = detail::read_u16_native(dst + i * 2u);
                detail::write_u16_native(dst + i * 2u, as<u16>(65535u - v));
            }
        } else if (sample_type == SampleType::U32) {
            for (usize i = 0; i < total_samples; ++i) {
                const u32 v = detail::read_u32_native(dst + i * 4u);
                detail::write_u32_native(dst + i * 4u, ~v);
            }
        }
    }

    if (will_expand) {
        return expand_palette(img, opts.allocator);
    }
    return img;
}

Result<Array<u8>> encode_tiff(const ImageView& view, const TiffEncodeOptions& opts, ArrayRef<u8> append_to_existing) {
    KEEL_TRY_ASSIGN(const detail::ValidatedView vv, detail::validate_image_view(view));
    if (view.channels != 1 && view.channels != 3 && view.channels != 4) [[unlikely]] {
        return ErrorCode::NotSupported;
    }

    KEEL_TRY_ASSIGN(const tiff_impl::SampleEncodingSpec spec, tiff_impl::map_sample_encoding(view.sample_type));
    KEEL_TRY_ASSIGN(const u16 comp_tag, tiff_impl::map_tiff_compression(opts.compression));

    const usize H = view.height;
    const usize W = view.width;
    const usize S = view.channels;
    const usize bps = spec.bytes_per_sample;
    const usize tight_row = vv.tight_row_bytes;
    if (H > Limits<usize>::kMax / tight_row) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }
    const usize raw_total = H * tight_row;

    ArenaTemp scratch = get_scratch(opts.allocator);
    Allocator* work_alloc = (raw_total <= 8ULL * 1024 * 1024) ? scratch.allocator() : nullptr;

    // Extract little-endian chunky scanlines.
    Array<u8> raw_bytes(raw_total, u8{0}, work_alloc);
    for (usize r = 0; r < H; ++r) {
        mem_copy(raw_bytes.data() + r * tight_row, view.pixels.data() + r * vv.stride_bytes, tight_row);
    }
    if (bps > 1u && detail::is_host_big_endian()) {
        u8* p = raw_bytes.data();
        const usize n_samples = H * W * S;
        if (bps == 2u) {
            for (usize i = 0; i < n_samples; ++i) {
                detail::write_u16_native(p + i * 2u, byteswap(detail::read_u16_native(p + i * 2u)));
            }
        } else if (bps == 4u) {
            for (usize i = 0; i < n_samples; ++i) {
                detail::write_u32_native(p + i * 4u, byteswap(detail::read_u32_native(p + i * 4u)));
            }
        } else if (bps == 8u) {
            for (usize i = 0; i < n_samples; ++i) {
                detail::write_u64_native(p + i * 8u, byteswap(detail::read_u64_native(p + i * 8u)));
            }
        }
    }

    const bool use_h_pred = (comp_tag == 5u || comp_tag == 8u) && (spec.sample_format != 3u) &&
                            (bps == 1u || bps == 2u || bps == 4u);
    if (use_h_pred) {
        tiff_impl::apply_horizontal_diff_le(raw_bytes.data(), H, W, S, bps);
    }
    const u16 predictor = use_h_pred ? u16{2} : u16{1};

    Array<u8> strip(work_alloc);
    switch (opts.compression) {
        case TiffCompression::None:
            strip = keel::move(raw_bytes);
            break;
        case TiffCompression::PackBits:
            strip = tiff_impl::encode_packbits(raw_bytes.data(), raw_bytes.size(), work_alloc);
            break;
        case TiffCompression::Lzw:
            strip = tiff_impl::encode_lzw(raw_bytes.data(), raw_bytes.size(), scratch.arena(), work_alloc);
            break;
        case TiffCompression::Deflate:
            strip = keel::compress::zlib_compress(raw_bytes.as_ref(), 6, work_alloc);
            break;
    }

    constexpr u64 kClassicLimit = 0xFFFFFFF0ULL;
    Array<u8> out(opts.allocator);
    bool big_tiff = opts.bigtiff;
    usize prev_next_slot = 0;

    if (!append_to_existing.empty()) {
        KEEL_TRY_ASSIGN(const tiff_impl::ByteReader ex_br, tiff_impl::open_tiff(append_to_existing));
        if (ex_br.big_endian) [[unlikely]] {
            return ErrorCode::NotSupported;
        }
        const u64 projected = as<u64>(append_to_existing.size()) + as<u64>(strip.size()) + 512ULL;
        big_tiff = opts.bigtiff || ex_br.is_big_tiff || (projected > kClassicLimit);

        if (!ex_br.is_big_tiff && big_tiff) {
            KEEL_TRY_ASSIGN(prev_next_slot,
                            tiff_impl::upgrade_classic_le_to_bigtiff(ex_br, out, scratch.arena()));
        } else {
            out.reserve(append_to_existing.size() + strip.size() + 320u);
            out.append(append_to_existing);

            prev_next_slot = big_tiff ? 8u : 4u;
            KEEL_TRY_ASSIGN(u64 off, tiff_impl::first_ifd_offset(ex_br));
            u32 pages = 0;
            while (off != 0) {
                if (++pages > tiff_impl::kMaxIfds) [[unlikely]] {
                    return ErrorCode::InvalidData;
                }
                u64 next_off = 0;
                usize slot = 0;
                KEEL_TRY(tiff_impl::step_ifd(ex_br, off, &next_off, &slot));
                prev_next_slot = slot;
                off = next_off;
            }
        }
    } else {
        const u64 projected = 16ULL + as<u64>(strip.size()) + 512ULL;
        big_tiff = opts.bigtiff || (projected > kClassicLimit);
        out.reserve((big_tiff ? 16u : 8u) + strip.size() + 320u);
        if (big_tiff) {
            out.resize(16u, u8{0});
            out[0] = 'I';
            out[1] = 'I';
            tiff_impl::write_u16_le_at(out, 2, 43u);
            tiff_impl::write_u16_le_at(out, 4, 8u);
            tiff_impl::write_u16_le_at(out, 6, 0u);
            prev_next_slot = 8u;
        } else {
            out.resize(8u, u8{0});
            out[0] = 'I';
            out[1] = 'I';
            tiff_impl::write_u16_le_at(out, 2, 42u);
            prev_next_slot = 4u;
        }
    }

    if ((out.size() & 1u) != 0u) {
        out.push_back(0u);
    }
    const u64 strip_offset = out.size();
    out.append(strip.as_ref());
    if ((out.size() & 1u) != 0u) {
        out.push_back(0u);
    }

    const u16 photometric = (S == 1u) ? u16{1} : u16{2};
    const usize inline_cap = big_tiff ? 8u : 4u;
    const usize bps_total_bytes = S * 2u;

    tiff_impl::RawIfdEntry bps_entry{.tag = 258u, .type = 3u, .count = S};
    if (bps_total_bytes <= inline_cap) {
        for (usize s = 0; s < S; ++s) {
            bps_entry.inline_bytes[s * 2u + 0u] = as<u8>(spec.bits_per_sample & 0xFFu);
            bps_entry.inline_bytes[s * 2u + 1u] = as<u8>((spec.bits_per_sample >> 8) & 0xFFu);
        }
    } else {
        bps_entry.is_external = true;
        bps_entry.external_offset = out.size();
        for (usize s = 0; s < S; ++s) {
            detail::push_u16_le(out, spec.bits_per_sample);
        }
        if ((out.size() & 1u) != 0u) {
            out.push_back(0u);
        }
    }

    // Emit tags in strictly ascending tag-number order per TIFF 6.0 specification.
    tiff_impl::RawIfdEntry entries[13]{};
    usize n_entries = 0;
    entries[n_entries++] = tiff_impl::make_u32_entry(256u, view.width);
    entries[n_entries++] = tiff_impl::make_u32_entry(257u, view.height);
    entries[n_entries++] = bps_entry;
    entries[n_entries++] = tiff_impl::make_u16_entry(259u, comp_tag);
    entries[n_entries++] = tiff_impl::make_u16_entry(262u, photometric);
    entries[n_entries++] = tiff_impl::make_offset_or_size_entry(273u, strip_offset, big_tiff);
    entries[n_entries++] = tiff_impl::make_u16_entry(277u, view.channels);
    entries[n_entries++] = tiff_impl::make_u32_entry(278u, view.height);
    entries[n_entries++] = tiff_impl::make_offset_or_size_entry(279u, strip.size(), big_tiff);
    entries[n_entries++] = tiff_impl::make_u16_entry(284u, 1u); // PlanarConfiguration = Chunky
    if (predictor != 1u) {
        entries[n_entries++] = tiff_impl::make_u16_entry(317u, predictor);
    }
    if (S == 4u) {
        // ExtraSamples = 2 (unassociated alpha) so external viewers recognize RGBA cleanly.
        entries[n_entries++] = tiff_impl::make_u16_entry(338u, 2u);
    }
    entries[n_entries++] = tiff_impl::make_u16_entry(339u, spec.sample_format);

    tiff_impl::emit_ifd(out, ArrayRef<tiff_impl::RawIfdEntry>(entries, n_entries), big_tiff, prev_next_slot);
    return out;
}

} // namespace keel::imagefile
