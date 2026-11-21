#include "image_internal.h"

namespace keel::imagefile {

namespace {
namespace jpeg_impl {

constexpr u8 kZigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// ITU-T T.81 Annex K.1 Luminance quantization table (spatial 8x8 order).
constexpr u8 kStdLumaQ[64] = {
    16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
    14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
    18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99,
};

// ITU-T T.81 Annex K.1 Chrominance quantization table (spatial 8x8 order).
constexpr u8 kStdChromaQ[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
    99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

// Standard Huffman tables (ITU-T T.81 Annex K.3).
constexpr u8 kDcBitsLuma[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr u8 kDcValLuma[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

constexpr u8 kDcBitsChroma[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
constexpr u8 kDcValChroma[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

constexpr u8 kAcBitsLuma[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125};
constexpr u8 kAcValLuma[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3,
    0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
};

constexpr u8 kAcBitsChroma[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119};
constexpr u8 kAcValChroma[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1,
    0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,
    0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA,
    0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA,
};

struct DctBasis {
    f32 m[8][8]; // m[u][x] = 0.5 * c(u) * cos((2*x + 1) * u * pi / 16)
};

consteval f64 cos_pi_over_16(int k_raw) {
    constexpr f64 kPi = 3.14159265358979323846264338327950288;
    int k = k_raw % 32;
    if (k < 0) k += 32;
    if (k > 16) k = 32 - k;
    if (k == 0) return 1.0;
    if (k == 8) return 0.0;
    if (k == 16) return -1.0;
    f64 sign = 1.0;
    if (k > 8) {
        k = 16 - k;
        sign = -1.0;
    }
    const f64 theta = as<f64>(k) * (kPi / 16.0);
    const f64 t2 = theta * theta;
    f64 term = 1.0;
    f64 sum = 1.0;
    for (int n = 1; n <= 12; ++n) {
        term *= -t2 / as<f64>((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sign * sum;
}

consteval DctBasis build_dct_basis() {
    DctBasis b{};
    constexpr f64 kInvSqrt2 = 0.707106781186547524400844362104849039;
    for (int u = 0; u < 8; ++u) {
        const f64 cu = (u == 0) ? kInvSqrt2 : 1.0;
        for (int x = 0; x < 8; ++x) {
            b.m[u][x] = as<f32>(0.5 * cu * cos_pi_over_16((2 * x + 1) * u));
        }
    }
    return b;
}

constexpr DctBasis kDct = build_dct_basis();

// Separable 2D IDCT: out = M^T * in * M
void idct8x8(const f32 in[64], f32 out[64]) noexcept {
    f32 tmp[64];
    for (int v = 0; v < 8; ++v) {
        for (int x = 0; x < 8; ++x) {
            f32 sum = 0.0f;
            for (int u = 0; u < 8; ++u) {
                sum += in[v * 8 + u] * kDct.m[u][x];
            }
            tmp[v * 8 + x] = sum;
        }
    }
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            f32 sum = 0.0f;
            for (int v = 0; v < 8; ++v) {
                sum += kDct.m[v][y] * tmp[v * 8 + x];
            }
            out[y * 8 + x] = sum;
        }
    }
}

// Separable 2D FDCT: out = M * in * M^T
void fdct8x8(const f32 in[64], f32 out[64]) noexcept {
    f32 tmp[64];
    for (int y = 0; y < 8; ++y) {
        for (int u = 0; u < 8; ++u) {
            f32 sum = 0.0f;
            for (int x = 0; x < 8; ++x) {
                sum += in[y * 8 + x] * kDct.m[u][x];
            }
            tmp[y * 8 + u] = sum;
        }
    }
    for (int v = 0; v < 8; ++v) {
        for (int u = 0; u < 8; ++u) {
            f32 sum = 0.0f;
            for (int y = 0; y < 8; ++y) {
                sum += kDct.m[v][y] * tmp[y * 8 + u];
            }
            out[v * 8 + u] = sum;
        }
    }
}

[[nodiscard]] constexpr u8 clamp_u8(int v) noexcept {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return as<u8>(v);
}

[[nodiscard]] constexpr i16 round_to_i16(f32 x) noexcept {
    return (x >= 0.0f) ? as<i16>(x + 0.5f) : as<i16>(x - 0.5f);
}

[[nodiscard]] constexpr bool is_sof_marker(u8 m) noexcept { return m == 0xC0u || m == 0xC1u || m == 0xC2u; }

[[nodiscard]] constexpr bool is_unsupported_sof_marker(u8 m) noexcept {
    return (m >= 0xC3u && m <= 0xCFu && m != 0xC4u && m != 0xC8u && m != 0xCCu);
}

[[nodiscard]] constexpr bool is_standalone_marker(u8 m) noexcept {
    return m == 0x00u || m == 0x01u || (m >= 0xD0u && m <= 0xD8u);
}

struct DecHuffTable {
    u8 counts[16]{};
    u8 values[256]{};
    u16 num_values = 0;
    u32 min_code[17]{};
    u32 max_code[17]{};
    int val_ptr[17]{};
    bool populated = false;

    void set_default(const u8 default_counts[16], const u8* default_vals, u16 nvals) noexcept {
        mem_copy(counts, default_counts, 16);
        mem_copy(values, default_vals, nvals);
        num_values = nvals;
        populated = true;
    }

    [[nodiscard]] bool build() noexcept {
        u32 code = 0;
        int ptr = 0;
        for (int len = 1; len <= 16; ++len) {
            val_ptr[len] = ptr;
            const u32 cnt = counts[len - 1];
            if (cnt > 0) {
                if (code + cnt > (1u << len)) [[unlikely]] {
                    return false;
                }
                min_code[len] = code;
                max_code[len] = code + cnt - 1u;
                code += cnt;
                ptr += as<int>(cnt);
            } else {
                min_code[len] = 0xFFFFFFFFu;
                max_code[len] = 0;
            }
            code <<= 1;
        }
        return as<u16>(ptr) <= num_values;
    }
};

class JpegBitReader {
public:
    JpegBitReader(const u8* data, usize len) noexcept : m_data(data), m_len(len) {}

    void align_byte() noexcept {
        m_bit_buf = 0;
        m_bit_count = 0;
    }

    [[nodiscard]] u32 get_bits(int n) noexcept {
        if (n <= 0) return 0;
        while (m_bit_count < n) {
            if (m_pos >= m_len) {
                m_bit_buf <<= 8;
                m_bit_count += 8;
                continue;
            }
            u8 b = m_data[m_pos++];
            if (b == 0xFFu) {
                while (m_pos < m_len && m_data[m_pos] == 0xFFu) {
                    ++m_pos;
                }
                if (m_pos >= m_len) {
                    b = 0;
                } else {
                    const u8 marker = m_data[m_pos++];
                    if (marker == 0x00u) {
                        b = 0xFFu; // Stuffed literal 0xFF
                    } else if (marker >= 0xD0u && marker <= 0xD7u) {
                        // Restart marker: skip and fetch next stream byte cleanly.
                        continue;
                    } else if (marker == 0xD9u) {
                        m_pos = m_len;
                        b = 0;
                    } else {
                        b = 0;
                    }
                }
            }
            m_bit_buf = (m_bit_buf << 8) | as<u64>(b);
            m_bit_count += 8;
        }
        const u32 val = as<u32>((m_bit_buf >> (m_bit_count - n)) & ((u64{1} << n) - 1u));
        m_bit_count -= n;
        return val;
    }

    [[nodiscard]] int decode_symbol(const DecHuffTable& ht) noexcept {
        u32 code = 0;
        for (int len = 1; len <= 16; ++len) {
            code = (code << 1) | get_bits(1);
            if (code <= ht.max_code[len] && code >= ht.min_code[len]) {
                const int idx = ht.val_ptr[len] + as<int>(code - ht.min_code[len]);
                if (idx >= 0 && idx < as<int>(ht.num_values)) {
                    return ht.values[idx];
                }
                return 0;
            }
        }
        return 0;
    }

    [[nodiscard]] int decode_value(int cat) noexcept {
        if (cat <= 0) return 0;
        if (cat > 15) cat = 15;
        const u32 bits = get_bits(cat);
        if (bits < (1u << (cat - 1))) {
            return as<int>(bits) - ((1 << cat) - 1);
        }
        return as<int>(bits);
    }

private:
    const u8* m_data = nullptr;
    usize m_len = 0;
    usize m_pos = 0;
    u64 m_bit_buf = 0;
    int m_bit_count = 0;
};

void decode_block(JpegBitReader& reader, const DecHuffTable& dc_ht, const DecHuffTable& ac_ht, const u16 q_table[64],
                  int& last_dc, f32 out_spatial[64]) noexcept {
    f32 freq[64]{};
    const int dc_cat = reader.decode_symbol(dc_ht);
    const int dc_diff = reader.decode_value(dc_cat);
    last_dc += dc_diff;
    freq[0] = as<f32>(last_dc * as<int>(q_table[0]));

    int k = 1;
    while (k < 64) {
        const int sym = reader.decode_symbol(ac_ht);
        if (sym == 0x00) break; // EOB
        if (sym == 0xF0) {      // ZRL (16 zeros)
            k += 16;
            continue;
        }
        const int run = (sym >> 4) & 0x0F;
        const int cat = sym & 0x0F;
        k += run;
        if (k >= 64) break;
        const int val = reader.decode_value(cat);
        const u8 zz = kZigzag[k];
        freq[zz] = as<f32>(val * as<int>(q_table[k]));
        ++k;
    }
    idct8x8(freq, out_spatial);
}

struct HuffCode {
    u16 code = 0;
    u8 len = 0;
};

struct HuffCodeTable {
    HuffCode codes[256]{};
};

[[nodiscard]] constexpr HuffCodeTable build_huffman_codes(const u8 bits[16], const u8* values, usize num_vals) noexcept {
    HuffCodeTable table{};
    u16 code = 0;
    usize val_idx = 0;
    for (int len = 1; len <= 16; ++len) {
        for (int i = 0; i < bits[len - 1]; ++i) {
            if (val_idx < num_vals) {
                const u8 val = values[val_idx++];
                table.codes[val] = HuffCode{.code = code, .len = as<u8>(len)};
                ++code;
            }
        }
        code = as<u16>(code << 1);
    }
    return table;
}

constexpr HuffCodeTable kEncDcLuma = build_huffman_codes(kDcBitsLuma, kDcValLuma, sizeof(kDcValLuma));
constexpr HuffCodeTable kEncAcLuma = build_huffman_codes(kAcBitsLuma, kAcValLuma, sizeof(kAcValLuma));
constexpr HuffCodeTable kEncDcChroma = build_huffman_codes(kDcBitsChroma, kDcValChroma, sizeof(kDcValChroma));
constexpr HuffCodeTable kEncAcChroma = build_huffman_codes(kAcBitsChroma, kAcValChroma, sizeof(kAcValChroma));

class JpegBitWriter {
public:
    explicit JpegBitWriter(Array<u8>& out) noexcept : m_out(out) {}

    void write_bits(u32 val, int n) {
        if (n <= 0) return;
        m_bit_buf |= (as<u64>(val) & ((u64{1} << n) - 1u)) << (64 - m_bit_count - n);
        m_bit_count += n;
        while (m_bit_count >= 8) {
            const u8 byte = as<u8>((m_bit_buf >> 56) & 0xFFu);
            m_out.push_back(byte);
            if (byte == 0xFFu) {
                m_out.push_back(0x00u);
            }
            m_bit_buf <<= 8;
            m_bit_count -= 8;
        }
    }

    void flush_bits() {
        if (m_bit_count > 0) {
            // Pad trailing bits with 1s per ITU-T T.81 F.1.2.3.
            const u64 pad_mask = ((u64{1} << (8 - m_bit_count)) - 1u) << (56);
            const u8 byte = as<u8>(((m_bit_buf | pad_mask) >> 56) & 0xFFu);
            m_out.push_back(byte);
            if (byte == 0xFFu) {
                m_out.push_back(0x00u);
            }
            m_bit_buf = 0;
            m_bit_count = 0;
        }
    }

private:
    Array<u8>& m_out;
    u64 m_bit_buf = 0;
    int m_bit_count = 0;
};

[[nodiscard]] constexpr int get_category(int val) noexcept {
    int abs_val = (val < 0) ? -val : val;
    int cat = 0;
    while (abs_val > 0) {
        ++cat;
        abs_val >>= 1;
    }
    return cat;
}

void write_category_bits(JpegBitWriter& writer, int val, int cat) {
    if (cat <= 0) return;
    int bits = val;
    if (val < 0) {
        bits = val + (1 << cat) - 1;
    }
    writer.write_bits(as<u32>(bits), cat);
}

void encode_block(JpegBitWriter& writer, const i16 block[64], int& last_dc, const HuffCodeTable& dc_codes,
                  const HuffCodeTable& ac_codes) {
    const int dc_diff = as<int>(block[0]) - last_dc;
    last_dc = block[0];

    const int dc_cat = get_category(dc_diff);
    writer.write_bits(dc_codes.codes[dc_cat].code, dc_codes.codes[dc_cat].len);
    write_category_bits(writer, dc_diff, dc_cat);

    int r = 0;
    for (int k = 1; k < 64; ++k) {
        const int ac = block[kZigzag[k]];
        if (ac == 0) {
            ++r;
        } else {
            while (r > 15) {
                writer.write_bits(ac_codes.codes[0xF0].code, ac_codes.codes[0xF0].len);
                r -= 16;
            }
            const int ac_cat = get_category(ac);
            const u8 sym = as<u8>((r << 4) | ac_cat);
            writer.write_bits(ac_codes.codes[sym].code, ac_codes.codes[sym].len);
            write_category_bits(writer, ac, ac_cat);
            r = 0;
        }
    }
    if (r > 0) {
        writer.write_bits(ac_codes.codes[0x00].code, ac_codes.codes[0x00].len);
    }
}

} // namespace jpeg_impl
} // namespace

Result<ImageInfo> peek_jpeg(ArrayRef<u8> bytes) {
    const u8* data = bytes.data();
    const usize len = bytes.size();
    if (len < 2) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    if (data[0] != 0xFFu || data[1] != 0xD8u) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (len < 4) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    usize p = 2;
    while (p < len) {
        if (data[p] != 0xFFu) {
            ++p;
            continue;
        }
        while (p < len && data[p] == 0xFFu) {
            ++p;
        }
        if (p >= len) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        const u8 marker = data[p++];

        if (jpeg_impl::is_standalone_marker(marker)) {
            continue;
        }
        if (marker == 0xD9u || marker == 0xDAu) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        if (jpeg_impl::is_unsupported_sof_marker(marker)) [[unlikely]] {
            return ErrorCode::NotSupported;
        }

        if (p + 2 > len) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        const u16 seg_len = detail::read_u16_be(data + p);
        if (seg_len < 2) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        if (p + seg_len > len) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }

        if (jpeg_impl::is_sof_marker(marker)) {
            if (seg_len < 8) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            const u8 precision = data[p + 2];
            const u32 h = detail::read_u16_be(data + p + 3);
            const u32 w = detail::read_u16_be(data + p + 5);
            const u8 ch = data[p + 7];
            if (w == 0 || h == 0 || ch == 0 || precision == 0) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            return ImageInfo{
                .format = ImageFormat::Jpeg,
                .width = w,
                .height = h,
                .channels = ch,
                .bits_per_sample = precision,
                .sample_type = (precision <= 8) ? SampleType::U8 : SampleType::U16,
                .has_palette = false,
                .page_count = 1,
            };
        }

        p += seg_len;
    }

    return ErrorCode::EndOfFile;
}

Result<Image> decode_jpeg(ArrayRef<u8> bytes, const ImageDecodeOptions& opts) {
    const u8* data = bytes.data();
    const usize len = bytes.size();
    if (len < 2) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    if (data[0] != 0xFFu || data[1] != 0xD8u) [[unlikely]] {
        return ErrorCode::InvalidData;
    }
    if (len < 4) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    struct ComponentInfo {
        u8 id = 0;
        u8 h_samp = 1;
        u8 v_samp = 1;
        u8 q_table_id = 0;
        u8 dc_table_id = 0;
        u8 ac_table_id = 0;
    };

    u32 width = 0;
    u32 height = 0;
    u16 bits = 0;
    u16 channels = 0;
    bool saw_sof = false;
    ComponentInfo comps[3]{};

    u16 q_tables[4][64]{};
    bool has_q_table[4]{};

    jpeg_impl::DecHuffTable dc_huff[4]{};
    jpeg_impl::DecHuffTable ac_huff[4]{};

    usize p = 2;
    usize sos_offset = 0;
    u16 restart_interval = 0;

    while (p < len) {
        if (data[p] != 0xFFu) {
            ++p;
            continue;
        }
        while (p < len && data[p] == 0xFFu) {
            ++p;
        }
        if (p >= len) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        const u8 marker = data[p++];

        if (jpeg_impl::is_standalone_marker(marker)) {
            continue;
        }
        if (marker == 0xD9u) [[unlikely]] {
            return ErrorCode::InvalidData; // EOI before SOS
        }
        if (jpeg_impl::is_unsupported_sof_marker(marker)) [[unlikely]] {
            return ErrorCode::NotSupported;
        }

        if (p + 2 > len) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }
        const u16 seg_len = detail::read_u16_be(data + p);
        if (seg_len < 2) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        if (p + seg_len > len) [[unlikely]] {
            return ErrorCode::EndOfFile;
        }

        const u8* seg = data + p + 2;
        const usize payload_len = as<usize>(seg_len) - 2u;

        if (jpeg_impl::is_sof_marker(marker)) {
            if (marker == 0xC2u) {
                // Progressive multi-scan SOF2 is only supported when single-scan compatible;
                // accept header here and decode scan below.
            }
            if (payload_len < 6) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            bits = seg[0];
            height = detail::read_u16_be(seg + 1);
            width = detail::read_u16_be(seg + 3);
            channels = seg[5];
            if (width == 0 || height == 0 || channels == 0) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            if (bits != 8 || (channels != 1 && channels != 3)) [[unlikely]] {
                return ErrorCode::NotSupported;
            }
            if (payload_len < 6u + as<usize>(channels) * 3u) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            for (u16 i = 0; i < channels; ++i) {
                const u8 id = seg[6 + i * 3];
                const u8 hv = seg[7 + i * 3];
                const u8 h_s = as<u8>((hv >> 4) & 0x0Fu);
                const u8 v_s = as<u8>(hv & 0x0Fu);
                const u8 qt = seg[8 + i * 3];
                if (h_s == 0 || h_s > 4 || v_s == 0 || v_s > 4 || qt >= 4) [[unlikely]] {
                    return ErrorCode::InvalidData;
                }
                comps[i] = ComponentInfo{
                    .id = id,
                    .h_samp = h_s,
                    .v_samp = v_s,
                    .q_table_id = qt,
                    .dc_table_id = as<u8>(i == 0 ? 0 : 1),
                    .ac_table_id = as<u8>(i == 0 ? 0 : 1),
                };
            }
            saw_sof = true;
        } else if (marker == 0xDAu) { // SOS
            if (!saw_sof || payload_len < 4) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            const u8 num_scan_comps = seg[0];
            if (num_scan_comps == 0 || payload_len < 1u + as<usize>(num_scan_comps) * 2u + 3u) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            for (u8 j = 0; j < num_scan_comps; ++j) {
                const u8 comp_id = seg[1 + j * 2];
                const u8 td_ta = seg[2 + j * 2];
                const u8 dc_id = as<u8>((td_ta >> 4) & 0x0Fu);
                const u8 ac_id = as<u8>(td_ta & 0x0Fu);
                if (dc_id >= 4 || ac_id >= 4) [[unlikely]] {
                    return ErrorCode::InvalidData;
                }
                for (u16 ci = 0; ci < channels; ++ci) {
                    if (comps[ci].id == comp_id) {
                        comps[ci].dc_table_id = dc_id;
                        comps[ci].ac_table_id = ac_id;
                        break;
                    }
                }
            }
            sos_offset = p + seg_len;
            break;
        } else if (marker == 0xDDu) { // DRI
            if (payload_len < 2) [[unlikely]] {
                return ErrorCode::InvalidData;
            }
            restart_interval = detail::read_u16_be(seg);
        } else if (marker == 0xDBu) { // DQT
            usize q_off = 0;
            while (q_off < payload_len) {
                const u8 info = seg[q_off++];
                const u8 table_id = as<u8>(info & 0x0Fu);
                const u8 pq = as<u8>((info >> 4) & 0x0Fu);
                if (table_id >= 4 || pq > 1) [[unlikely]] {
                    return ErrorCode::InvalidData;
                }
                if (pq == 1) {
                    if (q_off + 128 > payload_len) [[unlikely]] {
                        return ErrorCode::InvalidData;
                    }
                    for (int i = 0; i < 64; ++i) {
                        q_tables[table_id][i] = detail::read_u16_be(seg + q_off + i * 2);
                    }
                    has_q_table[table_id] = true;
                    q_off += 128;
                } else {
                    if (q_off + 64 > payload_len) [[unlikely]] {
                        return ErrorCode::InvalidData;
                    }
                    for (int i = 0; i < 64; ++i) {
                        q_tables[table_id][i] = seg[q_off + i];
                    }
                    has_q_table[table_id] = true;
                    q_off += 64;
                }
            }
        } else if (marker == 0xC4u) { // DHT
            usize h_off = 0;
            while (h_off < payload_len) {
                if (h_off + 17 > payload_len) [[unlikely]] {
                    return ErrorCode::InvalidData;
                }
                const u8 info = seg[h_off++];
                const u8 tc = as<u8>((info >> 4) & 0x0Fu);
                const u8 th = as<u8>(info & 0x0Fu);
                if (tc > 1 || th >= 4) [[unlikely]] {
                    return ErrorCode::InvalidData;
                }
                jpeg_impl::DecHuffTable& ht = (tc == 1) ? ac_huff[th] : dc_huff[th];
                mem_copy(ht.counts, seg + h_off, 16);
                h_off += 16;
                usize num_codes = 0;
                for (int i = 0; i < 16; ++i) {
                    num_codes += ht.counts[i];
                }
                if (num_codes > 256 || h_off + num_codes > payload_len) [[unlikely]] {
                    return ErrorCode::InvalidData;
                }
                if (num_codes > 0) {
                    mem_copy(ht.values, seg + h_off, num_codes);
                }
                ht.num_values = as<u16>(num_codes);
                ht.populated = true;
                h_off += num_codes;
            }
        }

        p += seg_len;
    }

    if (!saw_sof || sos_offset == 0) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }
    if (sos_offset >= len) [[unlikely]] {
        return ErrorCode::EndOfFile;
    }

    KEEL_TRY_ASSIGN(const usize total_bytes,
                    detail::validate_decode_allocation(width, height, channels, SampleType::U8, opts));

    // Populate default quantization & Huffman tables if omitted by abbreviated streams.
    if (!has_q_table[0]) {
        for (int k = 0; k < 64; ++k) {
            q_tables[0][k] = jpeg_impl::kStdLumaQ[jpeg_impl::kZigzag[k]];
        }
        has_q_table[0] = true;
    }
    if (!has_q_table[1]) {
        for (int k = 0; k < 64; ++k) {
            q_tables[1][k] = jpeg_impl::kStdChromaQ[jpeg_impl::kZigzag[k]];
        }
        has_q_table[1] = true;
    }
    for (u16 ci = 0; ci < channels; ++ci) {
        if (!has_q_table[comps[ci].q_table_id]) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
    }

    if (!dc_huff[0].populated) {
        dc_huff[0].set_default(jpeg_impl::kDcBitsLuma, jpeg_impl::kDcValLuma, sizeof(jpeg_impl::kDcValLuma));
    }
    if (!dc_huff[1].populated) {
        dc_huff[1].set_default(jpeg_impl::kDcBitsChroma, jpeg_impl::kDcValChroma, sizeof(jpeg_impl::kDcValChroma));
    }
    if (!ac_huff[0].populated) {
        ac_huff[0].set_default(jpeg_impl::kAcBitsLuma, jpeg_impl::kAcValLuma, sizeof(jpeg_impl::kAcValLuma));
    }
    if (!ac_huff[1].populated) {
        ac_huff[1].set_default(jpeg_impl::kAcBitsChroma, jpeg_impl::kAcValChroma, sizeof(jpeg_impl::kAcValChroma));
    }
    for (int i = 0; i < 4; ++i) {
        if (dc_huff[i].populated && !dc_huff[i].build()) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
        if (ac_huff[i].populated && !ac_huff[i].build()) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
    }
    for (u16 ci = 0; ci < channels; ++ci) {
        if (!dc_huff[comps[ci].dc_table_id].populated || !ac_huff[comps[ci].ac_table_id].populated) [[unlikely]] {
            return ErrorCode::InvalidData;
        }
    }

    u32 max_h = 1;
    u32 max_v = 1;
    for (u16 ci = 0; ci < channels; ++ci) {
        if (comps[ci].h_samp > max_h) max_h = comps[ci].h_samp;
        if (comps[ci].v_samp > max_v) max_v = comps[ci].v_samp;
    }

    const u32 mcu_px_w = max_h * 8u;
    const u32 mcu_px_h = max_v * 8u;
    const u32 mcu_cols = (width + mcu_px_w - 1u) / mcu_px_w;
    const u32 mcu_rows = (height + mcu_px_h - 1u) / mcu_px_h;

    Image img;
    img.width = width;
    img.height = height;
    img.channels = channels;
    img.sample_type = SampleType::U8;
    img.pixels = Array<u8>(total_bytes, u8{0}, opts.allocator);

    u8* dst = img.pixels.data();
    const usize row_stride = as<usize>(width) * as<usize>(channels);

    // Per-MCU spatial component buffers (at most 4x4 blocks = 32x32 floats per component).
    f32 mcu_planes[3][32 * 32]{};
    int last_dc[3] = {0, 0, 0};

    jpeg_impl::JpegBitReader reader(data + sos_offset, len - sos_offset);
    usize mcu_count = 0;

    for (u32 my = 0; my < mcu_rows; ++my) {
        for (u32 mx = 0; mx < mcu_cols; ++mx) {
            if (restart_interval > 0 && mcu_count > 0 && (mcu_count % restart_interval == 0)) {
                reader.align_byte();
                last_dc[0] = 0;
                last_dc[1] = 0;
                last_dc[2] = 0;
            }

            for (u16 ci = 0; ci < channels; ++ci) {
                const ComponentInfo& c = comps[ci];
                const u32 cw = as<u32>(c.h_samp) * 8u;
                f32* plane = mcu_planes[ci];
                for (u32 v = 0; v < c.v_samp; ++v) {
                    for (u32 h = 0; h < c.h_samp; ++h) {
                        f32 spatial[64];
                        jpeg_impl::decode_block(reader, dc_huff[c.dc_table_id], ac_huff[c.ac_table_id],
                                                q_tables[c.q_table_id], last_dc[ci], spatial);
                        const u32 br = v * 8u;
                        const u32 bc = h * 8u;
                        for (u32 by = 0; by < 8u; ++by) {
                            for (u32 bx = 0; bx < 8u; ++bx) {
                                plane[(br + by) * cw + (bc + bx)] = spatial[by * 8u + bx];
                            }
                        }
                    }
                }
            }

            const u32 r0 = my * mcu_px_h;
            const u32 c0 = mx * mcu_px_w;
            const u32 ly_end = (r0 + mcu_px_h <= height) ? mcu_px_h : (height - r0);
            const u32 lx_end = (c0 + mcu_px_w <= width) ? mcu_px_w : (width - c0);

            if (channels == 1) {
                const u32 y_stride = as<u32>(comps[0].h_samp) * 8u;
                for (u32 ly = 0; ly < ly_end; ++ly) {
                    const u32 y_r = (comps[0].v_samp == max_v) ? ly : ((ly * comps[0].v_samp) / max_v);
                    u8* out_row = dst + as<usize>(r0 + ly) * row_stride + as<usize>(c0);
                    for (u32 lx = 0; lx < lx_end; ++lx) {
                        const u32 y_c = (comps[0].h_samp == max_h) ? lx : ((lx * comps[0].h_samp) / max_h);
                        const f32 y_val = mcu_planes[0][y_r * y_stride + y_c] + 128.5f;
                        out_row[lx] = jpeg_impl::clamp_u8(as<int>(y_val));
                    }
                }
            } else {
                const u32 y_w = as<u32>(comps[0].h_samp) * 8u;
                const u32 cb_w = as<u32>(comps[1].h_samp) * 8u;
                const u32 cr_w = as<u32>(comps[2].h_samp) * 8u;
                for (u32 ly = 0; ly < ly_end; ++ly) {
                    const u32 y_r = (comps[0].v_samp == max_v) ? ly : ((ly * comps[0].v_samp) / max_v);
                    const u32 cb_r = (comps[1].v_samp == max_v) ? ly : ((ly * comps[1].v_samp) / max_v);
                    const u32 cr_r = (comps[2].v_samp == max_v) ? ly : ((ly * comps[2].v_samp) / max_v);
                    u8* out_row = dst + as<usize>(r0 + ly) * row_stride + as<usize>(c0) * 3u;
                    for (u32 lx = 0; lx < lx_end; ++lx) {
                        const u32 y_c = (comps[0].h_samp == max_h) ? lx : ((lx * comps[0].h_samp) / max_h);
                        const u32 cb_c = (comps[1].h_samp == max_h) ? lx : ((lx * comps[1].h_samp) / max_h);
                        const u32 cr_c = (comps[2].h_samp == max_h) ? lx : ((lx * comps[2].h_samp) / max_h);

                        const f32 y_val = mcu_planes[0][y_r * y_w + y_c];
                        const f32 cb_val = mcu_planes[1][cb_r * cb_w + cb_c];
                        const f32 cr_val = mcu_planes[2][cr_r * cr_w + cr_c];

                        const int r = as<int>(y_val + 1.402f * cr_val + 128.5f);
                        const int g = as<int>(y_val - 0.344136f * cb_val - 0.714136f * cr_val + 128.5f);
                        const int b = as<int>(y_val + 1.772f * cb_val + 128.5f);

                        out_row[lx * 3u + 0] = jpeg_impl::clamp_u8(r);
                        out_row[lx * 3u + 1] = jpeg_impl::clamp_u8(g);
                        out_row[lx * 3u + 2] = jpeg_impl::clamp_u8(b);
                    }
                }
            }

            ++mcu_count;
        }
    }

    return img;
}

Result<Array<u8>> encode_jpeg(const ImageView& view, const JpegEncodeOptions& opts) {
    KEEL_TRY_ASSIGN(const detail::ValidatedView vv, detail::validate_image_view(view));

    if (view.sample_type != SampleType::U8) [[unlikely]] {
        return ErrorCode::NotSupported;
    }
    if (view.channels != 1 && view.channels != 3) [[unlikely]] {
        return ErrorCode::NotSupported;
    }
    if (view.width > 65535u || view.height > 65535u) [[unlikely]] {
        return ErrorCode::OutOfRange;
    }

    int q = opts.quality;
    if (q < 1) {
        q = 1;
    } else if (q > 100) {
        q = 100;
    }
    const f32 scale = (q < 50) ? (5000.0f / as<f32>(q)) : (200.0f - 2.0f * as<f32>(q));

    u8 luma_q[64]{};
    u8 chroma_q[64]{};
    f32 inv_luma_q[64]{};
    f32 inv_chroma_q[64]{};
    for (int i = 0; i < 64; ++i) {
        int vl = as<int>((as<f32>(jpeg_impl::kStdLumaQ[i]) * scale + 50.0f) / 100.0f);
        int vc = as<int>((as<f32>(jpeg_impl::kStdChromaQ[i]) * scale + 50.0f) / 100.0f);
        if (vl < 1) vl = 1;
        if (vl > 255) vl = 255;
        if (vc < 1) vc = 1;
        if (vc > 255) vc = 255;
        luma_q[i] = as<u8>(vl);
        chroma_q[i] = as<u8>(vc);
        inv_luma_q[i] = 1.0f / as<f32>(vl);
        inv_chroma_q[i] = 1.0f / as<f32>(vc);
    }

    const usize approx_bytes =
        640u + (as<usize>(view.width) * as<usize>(view.height) * view.channels) / 4u;
    Array<u8> out(opts.allocator);
    out.reserve(approx_bytes);

    // 1. SOI (0xFFD8)
    out.push_back(0xFFu);
    out.push_back(0xD8u);

    // 2. APP0 (0xFFE0) - JFIF 1.01
    constexpr u8 kJfifApp0[18] = {
        0xFFu, 0xE0u, 0x00u, 0x10u, 'J',   'F',   'I',   'F',   0x00u,
        0x01u, 0x01u, 0x01u, 0x00u, 0x48u, 0x00u, 0x48u, 0x00u, 0x00u,
    };
    out.append(ArrayRef<u8>(kJfifApp0, sizeof(kJfifApp0)));

    // 3. DQT (0xFFDB) - Quantization tables in ITU-T T.81 zigzag order
    out.push_back(0xFFu);
    out.push_back(0xDBu);
    detail::push_u16_be(out, as<u16>((view.channels == 1) ? 67u : 132u));
    out.push_back(0x00u); // Table 0 (8-bit)
    for (int k = 0; k < 64; ++k) {
        out.push_back(luma_q[jpeg_impl::kZigzag[k]]);
    }
    if (view.channels == 3) {
        out.push_back(0x01u); // Table 1 (8-bit)
        for (int k = 0; k < 64; ++k) {
            out.push_back(chroma_q[jpeg_impl::kZigzag[k]]);
        }
    }

    // 4. SOF0 (0xFFC0) - Baseline DCT Frame Header (4:4:4 1x1 sampling)
    out.push_back(0xFFu);
    out.push_back(0xC0u);
    detail::push_u16_be(out, as<u16>(8u + 3u * view.channels));
    out.push_back(8u); // 8-bit sample precision
    detail::push_u16_be(out, as<u16>(view.height));
    detail::push_u16_be(out, as<u16>(view.width));
    out.push_back(as<u8>(view.channels));
    if (view.channels == 1) {
        out.push_back(1u);
        out.push_back(0x11u);
        out.push_back(0u);
    } else {
        out.push_back(1u);
        out.push_back(0x11u);
        out.push_back(0u);
        out.push_back(2u);
        out.push_back(0x11u);
        out.push_back(1u);
        out.push_back(3u);
        out.push_back(0x11u);
        out.push_back(1u);
    }

    // 5. DHT (0xFFC4) - Huffman Tables
    auto emit_dht = [&](u8 tc, u8 th, const u8 bits[16], const u8* vals, usize n_vals) {
        out.push_back(0xFFu);
        out.push_back(0xC4u);
        detail::push_u16_be(out, as<u16>(19u + n_vals));
        out.push_back(as<u8>((tc << 4) | th));
        out.append(ArrayRef<u8>(bits, 16));
        out.append(ArrayRef<u8>(vals, n_vals));
    };

    emit_dht(0, 0, jpeg_impl::kDcBitsLuma, jpeg_impl::kDcValLuma, sizeof(jpeg_impl::kDcValLuma));
    emit_dht(1, 0, jpeg_impl::kAcBitsLuma, jpeg_impl::kAcValLuma, sizeof(jpeg_impl::kAcValLuma));
    if (view.channels == 3) {
        emit_dht(0, 1, jpeg_impl::kDcBitsChroma, jpeg_impl::kDcValChroma, sizeof(jpeg_impl::kDcValChroma));
        emit_dht(1, 1, jpeg_impl::kAcBitsChroma, jpeg_impl::kAcValChroma, sizeof(jpeg_impl::kAcValChroma));
    }

    // 6. SOS (0xFFDA) - Scan Header
    out.push_back(0xFFu);
    out.push_back(0xDAu);
    detail::push_u16_be(out, as<u16>(6u + 2u * view.channels));
    out.push_back(as<u8>(view.channels));
    if (view.channels == 1) {
        out.push_back(1u);
        out.push_back(0x00u);
    } else {
        out.push_back(1u);
        out.push_back(0x00u);
        out.push_back(2u);
        out.push_back(0x11u);
        out.push_back(3u);
        out.push_back(0x11u);
    }
    out.push_back(0u);  // Ss
    out.push_back(63u); // Se
    out.push_back(0u);  // Ah/Al

    // 7. Entropy-coded 8x8 MCUs
    jpeg_impl::JpegBitWriter bit_writer(out);
    const u32 mcu_cols = (view.width + 7u) / 8u;
    const u32 mcu_rows = (view.height + 7u) / 8u;
    const u8* src_base = view.pixels.data();
    const usize src_stride = vv.stride_bytes;

    int last_dc_y = 0;
    int last_dc_cb = 0;
    int last_dc_cr = 0;

    f32 spatial_y[64];
    f32 spatial_cb[64];
    f32 spatial_cr[64];
    f32 dct[64];
    i16 quant_block[64];

    for (u32 my = 0; my < mcu_rows; ++my) {
        for (u32 mx = 0; mx < mcu_cols; ++mx) {
            if (view.channels == 1) {
                for (u32 y = 0; y < 8u; ++y) {
                    const u32 py = (my * 8u + y < view.height) ? (my * 8u + y) : (view.height - 1u);
                    const u8* row = src_base + as<usize>(py) * src_stride;
                    for (u32 x = 0; x < 8u; ++x) {
                        const u32 px = (mx * 8u + x < view.width) ? (mx * 8u + x) : (view.width - 1u);
                        spatial_y[y * 8u + x] = as<f32>(row[px]) - 128.0f;
                    }
                }

                jpeg_impl::fdct8x8(spatial_y, dct);
                for (int i = 0; i < 64; ++i) {
                    quant_block[i] = jpeg_impl::round_to_i16(dct[i] * inv_luma_q[i]);
                }
                jpeg_impl::encode_block(bit_writer, quant_block, last_dc_y, jpeg_impl::kEncDcLuma,
                                        jpeg_impl::kEncAcLuma);
            } else {
                for (u32 y = 0; y < 8u; ++y) {
                    const u32 py = (my * 8u + y < view.height) ? (my * 8u + y) : (view.height - 1u);
                    const u8* row = src_base + as<usize>(py) * src_stride;
                    for (u32 x = 0; x < 8u; ++x) {
                        const u32 px = (mx * 8u + x < view.width) ? (mx * 8u + x) : (view.width - 1u);
                        const f32 r = as<f32>(row[px * 3u + 0]);
                        const f32 g = as<f32>(row[px * 3u + 1]);
                        const f32 b = as<f32>(row[px * 3u + 2]);

                        const f32 y_val = 0.29900f * r + 0.58700f * g + 0.11400f * b;
                        const f32 cb_val = -0.16874f * r - 0.33126f * g + 0.50000f * b;
                        const f32 cr_val = 0.50000f * r - 0.41869f * g - 0.08131f * b;

                        spatial_y[y * 8u + x] = y_val - 128.0f;
                        spatial_cb[y * 8u + x] = cb_val;
                        spatial_cr[y * 8u + x] = cr_val;
                    }
                }

                jpeg_impl::fdct8x8(spatial_y, dct);
                for (int i = 0; i < 64; ++i) {
                    quant_block[i] = jpeg_impl::round_to_i16(dct[i] * inv_luma_q[i]);
                }
                jpeg_impl::encode_block(bit_writer, quant_block, last_dc_y, jpeg_impl::kEncDcLuma,
                                        jpeg_impl::kEncAcLuma);

                jpeg_impl::fdct8x8(spatial_cb, dct);
                for (int i = 0; i < 64; ++i) {
                    quant_block[i] = jpeg_impl::round_to_i16(dct[i] * inv_chroma_q[i]);
                }
                jpeg_impl::encode_block(bit_writer, quant_block, last_dc_cb, jpeg_impl::kEncDcChroma,
                                        jpeg_impl::kEncAcChroma);

                jpeg_impl::fdct8x8(spatial_cr, dct);
                for (int i = 0; i < 64; ++i) {
                    quant_block[i] = jpeg_impl::round_to_i16(dct[i] * inv_chroma_q[i]);
                }
                jpeg_impl::encode_block(bit_writer, quant_block, last_dc_cr, jpeg_impl::kEncDcChroma,
                                        jpeg_impl::kEncAcChroma);
            }
        }
    }

    bit_writer.flush_bits();

    // 8. EOI (0xFFD9)
    out.push_back(0xFFu);
    out.push_back(0xD9u);

    return out;
}

} // namespace keel::imagefile
