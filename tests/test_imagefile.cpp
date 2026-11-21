#include "keel/imagefile.h"

#include "keel/arena.h"
#include "keel/fs.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace keel;
using namespace keel::imagefile;

#ifndef KEEL_IMAGEFILE_FIXTURES_DIR
#define KEEL_IMAGEFILE_FIXTURES_DIR "tests/fixtures"
#endif

namespace {

int g_tests_passed = 0;
int g_tests_failed = 0;

#define TEST_CHECK(cond)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                     \
            ++g_tests_failed;                                                                                          \
        } else {                                                                                                       \
            ++g_tests_passed;                                                                                          \
        }                                                                                                              \
    } while (0)

// Minimal CRC-32 & uncompressed ZLIB helpers for synthesizing raw PNG test vectors
// (Adam7 interlace, sub-byte bit depths, explicit filter bytes, CRC corruption).
u32 test_crc32(const u8* data, usize len) noexcept {
    u32 c = 0xFFFFFFFFu;
    for (usize i = 0; i < len; ++i) {
        c ^= data[i];
        for (int b = 0; b < 8; ++b) {
            c = (c >> 1) ^ ((c & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return ~c;
}

u32 test_adler32(const u8* data, usize len) noexcept {
    u32 s1 = 1u;
    u32 s2 = 0u;
    for (usize i = 0; i < len; ++i) {
        s1 = (s1 + data[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    return (s2 << 16) | s1;
}

Array<u8> make_zlib_stored(ArrayRef<u8> raw) {
    Array<u8> z;
    z.push_back(0x78u);
    z.push_back(0x01u);
    usize pos = 0;
    do {
        const usize rem = raw.size() - pos;
        const u16 blk = static_cast<u16>((rem > 65535u) ? 65535u : rem);
        const bool last = (pos + blk == raw.size());
        z.push_back(last ? 0x01u : 0x00u);
        z.push_back(static_cast<u8>(blk & 0xFFu));
        z.push_back(static_cast<u8>((blk >> 8) & 0xFFu));
        const u16 nlen = static_cast<u16>(~blk);
        z.push_back(static_cast<u8>(nlen & 0xFFu));
        z.push_back(static_cast<u8>((nlen >> 8) & 0xFFu));
        if (blk > 0) {
            z.append(raw.slice(pos, blk));
        }
        pos += blk;
    } while (pos < raw.size());

    const u32 adler = test_adler32(raw.data(), raw.size());
    z.push_back(static_cast<u8>((adler >> 24) & 0xFFu));
    z.push_back(static_cast<u8>((adler >> 16) & 0xFFu));
    z.push_back(static_cast<u8>((adler >> 8) & 0xFFu));
    z.push_back(static_cast<u8>(adler & 0xFFu));
    return z;
}

void append_png_chunk(Array<u8>& png, const char type[4], ArrayRef<u8> payload) {
    const u32 len = static_cast<u32>(payload.size());
    png.push_back(static_cast<u8>((len >> 24) & 0xFFu));
    png.push_back(static_cast<u8>((len >> 16) & 0xFFu));
    png.push_back(static_cast<u8>((len >> 8) & 0xFFu));
    png.push_back(static_cast<u8>(len & 0xFFu));

    const usize type_start = png.size();
    for (int i = 0; i < 4; ++i) {
        png.push_back(static_cast<u8>(type[i]));
    }
    png.append(payload);

    const u32 crc = test_crc32(png.data() + type_start, 4u + payload.size());
    png.push_back(static_cast<u8>((crc >> 24) & 0xFFu));
    png.push_back(static_cast<u8>((crc >> 16) & 0xFFu));
    png.push_back(static_cast<u8>((crc >> 8) & 0xFFu));
    png.push_back(static_cast<u8>(crc & 0xFFu));
}

Array<u8> build_raw_png(u32 w, u32 h, u8 bit_depth, u8 color_type, u8 interlace, ArrayRef<u8> raw_scanlines,
                        ArrayRef<u8> plte_rgb = {}) {
    Array<u8> png;
    const u8 sig[8] = {0x89u, 'P', 'N', 'G', 0x0Du, 0x0Au, 0x1Au, 0x0Au};
    png.append(ArrayRef<u8>(sig, 8));

    u8 ihdr[13] = {
        static_cast<u8>((w >> 24) & 0xFFu), static_cast<u8>((w >> 16) & 0xFFu),
        static_cast<u8>((w >> 8) & 0xFFu),  static_cast<u8>(w & 0xFFu),
        static_cast<u8>((h >> 24) & 0xFFu), static_cast<u8>((h >> 16) & 0xFFu),
        static_cast<u8>((h >> 8) & 0xFFu),  static_cast<u8>(h & 0xFFu),
        bit_depth,
        color_type,
        0u,
        0u,
        interlace,
    };
    append_png_chunk(png, "IHDR", ArrayRef<u8>(ihdr, 13));
    if (!plte_rgb.empty()) {
        append_png_chunk(png, "PLTE", plte_rgb);
    }
    Array<u8> idat = make_zlib_stored(raw_scanlines);
    append_png_chunk(png, "IDAT", idat);
    append_png_chunk(png, "IEND", ArrayRef<u8>{});
    return png;
}

Image make_test_imagefile(u32 w, u32 h, u16 ch, SampleType st) {
    Image img;
    img.width = w;
    img.height = h;
    img.channels = ch;
    img.sample_type = st;
    const usize n_samples = static_cast<usize>(w) * static_cast<usize>(h) * static_cast<usize>(ch);
    const usize bps = sample_byte_size(st);
    img.pixels = Array<u8>(n_samples * bps, u8{0});
    u8* p = img.pixels.data();

    u32 rng = 0x9E3779B9u ^ (w * 131u + h * 17u + ch * 7u + static_cast<u32>(st));
    for (usize i = 0; i < n_samples; ++i) {
        rng = rng * 1664525u + 1013904223u;
        switch (st) {
            case SampleType::U8: {
                p[i] = static_cast<u8>((i * 29u + (rng >> 24)) & 0xFFu);
                break;
            }
            case SampleType::U16: {
                const u16 v = static_cast<u16>((i * 257u + (rng >> 16)) & 0xFFFFu);
                std::memcpy(p + i * 2u, &v, 2);
                break;
            }
            case SampleType::U32: {
                const u32 v = rng ^ static_cast<u32>(i * 65537u);
                std::memcpy(p + i * 4u, &v, 4);
                break;
            }
            case SampleType::I8: {
                const i8 v = static_cast<i8>(static_cast<int>((i * 19u + (rng >> 24)) & 0xFFu) - 128);
                std::memcpy(p + i, &v, 1);
                break;
            }
            case SampleType::I16: {
                const i16 v = static_cast<i16>(static_cast<int>((i * 521u + (rng >> 16)) & 0xFFFFu) - 32768);
                std::memcpy(p + i * 2u, &v, 2);
                break;
            }
            case SampleType::I32: {
                const i32 v = static_cast<i32>(rng) - 1000000000;
                std::memcpy(p + i * 4u, &v, 4);
                break;
            }
            case SampleType::F32: {
                const float v = static_cast<float>(static_cast<int>(rng & 0xFFFFu) - 32768) * 0.03125f;
                std::memcpy(p + i * 4u, &v, 4);
                break;
            }
            case SampleType::F64: {
                const double v = static_cast<double>(static_cast<int>(rng & 0xFFFFFFu) - 8388608) * 0.0001220703125;
                std::memcpy(p + i * 8u, &v, 8);
                break;
            }
        }
    }
    return img;
}

// ============================================================================
// 1. PNG Tests
// ============================================================================

void test_png() {
    // Round-trip 8-bit and 16-bit across 1, 2 (Gray+Alpha), 3 (RGB), and 4 (RGBA) channels.
    for (SampleType st : {SampleType::U8, SampleType::U16}) {
        for (u16 ch : {u16{1}, u16{2}, u16{3}, u16{4}}) {
            Image src = make_test_imagefile(19, 13, ch, st);
            for (int lvl : {0, 6}) {
                auto enc = encode_png(src, PngEncodeOptions{.compression_level = lvl});
                TEST_CHECK(enc.is_ok());
                if (!enc) continue;

                TEST_CHECK(sniff_image_format(*enc) == ImageFormat::Png);
                auto info = peek_png(*enc);
                TEST_CHECK(info.is_ok());
                if (info) {
                    TEST_CHECK(info->format == ImageFormat::Png);
                    TEST_CHECK(info->width == 19u && info->height == 13u);
                    TEST_CHECK(info->channels == ch);
                    TEST_CHECK(info->bits_per_sample == sample_bit_depth(st));
                    TEST_CHECK(info->sample_type == st);
                    TEST_CHECK(!info->has_palette);
                }

                auto dec = decode_png(*enc);
                TEST_CHECK(dec.is_ok());
                if (dec) {
                    TEST_CHECK(dec->width == 19u && dec->height == 13u);
                    TEST_CHECK(dec->channels == ch);
                    TEST_CHECK(dec->sample_type == st);
                    TEST_CHECK(!dec->has_palette());
                    TEST_CHECK(dec->pixels == src.pixels);
                }
            }
        }
    }

    // Indexed PLTE PNG round-trip + expand_palette + opts.expand_indexed.
    {
        Image idx_img;
        idx_img.width = 6;
        idx_img.height = 4;
        idx_img.channels = 1;
        idx_img.sample_type = SampleType::U8;
        idx_img.palette_bits = 8;
        idx_img.pixels = Array<u8>(24, u8{0});
        for (usize i = 0; i < 24; ++i) {
            idx_img.pixels[i] = static_cast<u8>(i % 4u);
        }
        const u16 pal[12] = {
            10, 20, 30,    // 0
            250, 0, 15,    // 1
            0, 220, 40,    // 2
            100, 150, 255, // 3
        };
        idx_img.palette.append(ArrayRef<u16>(pal, 12));

        auto enc = encode_png(idx_img, PngEncodeOptions{.compression_level = 6, .palette = idx_img.palette});
        TEST_CHECK(enc.is_ok());
        if (enc) {
            auto info = peek_png(*enc);
            TEST_CHECK(info.is_ok() && info->has_palette && info->channels == 1u);

            auto dec = decode_png(*enc);
            TEST_CHECK(dec.is_ok());
            if (dec) {
                TEST_CHECK(dec->channels == 1u);
                TEST_CHECK(dec->sample_type == SampleType::U8);
                TEST_CHECK(dec->has_palette() && dec->palette_bits == 8u);
                TEST_CHECK(dec->palette_entry_count() == 4u);
                TEST_CHECK(dec->palette == idx_img.palette);
                TEST_CHECK(dec->pixels == idx_img.pixels);

                auto rgb = expand_palette(*dec);
                TEST_CHECK(rgb.is_ok());
                if (rgb) {
                    TEST_CHECK(rgb->channels == 3u && rgb->sample_type == SampleType::U8);
                    TEST_CHECK(!rgb->has_palette());
                    TEST_CHECK(rgb->pixels[0] == 10u && rgb->pixels[1] == 20u && rgb->pixels[2] == 30u);
                    TEST_CHECK(rgb->pixels[3] == 250u && rgb->pixels[4] == 0u && rgb->pixels[5] == 15u);

                    auto dec_exp = decode_png(*enc, ImageDecodeOptions{.expand_indexed = true});
                    TEST_CHECK(dec_exp.is_ok() && dec_exp->pixels == rgb->pixels);
                }
            }
        }
    }

    // All 5 PNG scanline filters (0=None, 1=Sub, 2=Up, 3=Average, 4=Paeth) on a 4x5 RGB image.
    {
        const u32 W = 4, H = 5;
        u8 expected[W * H * 3]{};
        for (usize i = 0; i < sizeof(expected); ++i) {
            expected[i] = static_cast<u8>((i * 37u + 11u) & 0xFFu);
        }
        const usize row_b = W * 3u;
        Array<u8> raw;
        auto paeth = [](int a, int b, int c) -> u8 {
            const int p = a + b - c;
            const int pa = (p >= a) ? (p - a) : (a - p);
            const int pb = (p >= b) ? (p - b) : (b - p);
            const int pc = (p >= c) ? (p - c) : (c - p);
            if (pa <= pb && pa <= pc) return static_cast<u8>(a);
            if (pb <= pc) return static_cast<u8>(b);
            return static_cast<u8>(c);
        };
        for (u32 r = 0; r < H; ++r) {
            const u8 ftype = static_cast<u8>(r % 5u);
            raw.push_back(ftype);
            const u8* cur = expected + r * row_b;
            const u8* prev = (r > 0) ? (expected + (r - 1u) * row_b) : nullptr;
            for (usize x = 0; x < row_b; ++x) {
                const u8 a = (x >= 3u) ? cur[x - 3u] : 0u;
                const u8 b = prev ? prev[x] : 0u;
                const u8 c = (prev && x >= 3u) ? prev[x - 3u] : 0u;
                u8 pred = 0;
                if (ftype == 1) pred = a;
                else if (ftype == 2) pred = b;
                else if (ftype == 3) pred = static_cast<u8>((static_cast<int>(a) + static_cast<int>(b)) >> 1);
                else if (ftype == 4) pred = paeth(a, b, c);
                raw.push_back(static_cast<u8>(cur[x] - pred));
            }
        }
        Array<u8> png = build_raw_png(W, H, 8, 2, 0, raw);
        auto dec = decode_png(png);
        TEST_CHECK(dec.is_ok() && dec->pixels == ArrayRef<u8>(expected, sizeof(expected)));
    }

    // Adam7 Interlaced PNG (5x5 8-bit grayscale, interlace=1).
    {
        const u32 W = 5, H = 5;
        u8 expected[25]{};
        for (u32 i = 0; i < 25; ++i) expected[i] = static_cast<u8>(i * 10u + 3u);

        const u32 x0[7] = {0, 4, 0, 2, 0, 1, 0};
        const u32 y0[7] = {0, 0, 4, 0, 2, 0, 1};
        const u32 dx[7] = {8, 8, 4, 4, 2, 2, 1};
        const u32 dy[7] = {8, 8, 8, 4, 4, 2, 2};

        Array<u8> raw;
        for (int p = 0; p < 7; ++p) {
            if (x0[p] >= W || y0[p] >= H) continue;
            for (u32 y = y0[p]; y < H; y += dy[p]) {
                raw.push_back(0u); // Filter = None
                for (u32 x = x0[p]; x < W; x += dx[p]) {
                    raw.push_back(expected[y * W + x]);
                }
            }
        }
        Array<u8> png = build_raw_png(W, H, 8, 0, 1, raw);
        auto dec = decode_png(png);
        TEST_CHECK(dec.is_ok() && dec->pixels == ArrayRef<u8>(expected, 25));
    }

    // 4-bit grayscale PNG scaling (0..15 -> 0..255 via * 17).
    {
        const u8 raw_4bit[3] = {0u, 0x0Fu, 0x5Au}; // row of 4 pixels: 0, 15, 5, 10
        Array<u8> png = build_raw_png(4, 1, 4, 0, 0, ArrayRef<u8>(raw_4bit, 3));
        auto dec = decode_png(png);
        TEST_CHECK(dec.is_ok());
        if (dec) {
            TEST_CHECK(dec->pixels[0] == 0u && dec->pixels[1] == 255u && dec->pixels[2] == 85u &&
                       dec->pixels[3] == 170u);
        }
    }

    // Non-zero row_stride_bytes sub-view encoding.
    {
        u8 padded[3 * 16]{};
        for (u32 r = 0; r < 3; ++r) {
            for (u32 c = 0; c < 10; ++c) {
                padded[r * 16u + c] = static_cast<u8>(r * 30u + c + 1u);
            }
        }
        ImageView sub_view{
            .width = 10,
            .height = 3,
            .channels = 1,
            .sample_type = SampleType::U8,
            .row_stride_bytes = 16,
            .pixels = ArrayRef<u8>(padded, sizeof(padded)),
        };
        auto enc = encode_png(sub_view);
        TEST_CHECK(enc.is_ok());
        if (enc) {
            auto dec = decode_png(*enc);
            TEST_CHECK(dec.is_ok() && dec->width == 10u && dec->height == 3u);
            if (dec) {
                for (u32 r = 0; r < 3; ++r) {
                    for (u32 c = 0; c < 10; ++c) {
                        TEST_CHECK(dec->pixels[r * 10u + c] == static_cast<u8>(r * 30u + c + 1u));
                    }
                }
            }
        }
    }
}

// ============================================================================
// 2. JPEG Tests
// ============================================================================

void test_jpeg() {
    // Smooth RGB gradient round-trip at quality 95 vs quality 25.
    {
        Image grad;
        grad.width = 32;
        grad.height = 24;
        grad.channels = 3;
        grad.sample_type = SampleType::U8;
        grad.pixels = Array<u8>(32 * 24 * 3, u8{0});
        for (u32 r = 0; r < 24; ++r) {
            for (u32 c = 0; c < 32; ++c) {
                const usize idx = (r * 32u + c) * 3u;
                grad.pixels[idx + 0] = static_cast<u8>(c * 7u + 10u);
                grad.pixels[idx + 1] = static_cast<u8>(r * 9u + 15u);
                grad.pixels[idx + 2] = static_cast<u8>(180u - c * 3u);
            }
        }

        auto hi = encode_jpeg(grad, JpegEncodeOptions{.quality = 95});
        auto lo = encode_jpeg(grad, JpegEncodeOptions{.quality = 20});
        TEST_CHECK(hi.is_ok() && lo.is_ok());
        if (hi && lo) {
            TEST_CHECK(hi->size() > lo->size());
            TEST_CHECK(sniff_image_format(*hi) == ImageFormat::Jpeg);
            auto info = peek_jpeg(*hi);
            TEST_CHECK(info.is_ok() && info->width == 32u && info->height == 24u && info->channels == 3u);

            auto dec_hi = decode_jpeg(*hi);
            auto dec_lo = decode_jpeg(*lo);
            TEST_CHECK(dec_hi.is_ok() && dec_lo.is_ok());
            if (dec_hi && dec_lo) {
                double err_hi = 0.0;
                double err_lo = 0.0;
                for (usize i = 0; i < grad.pixels.size(); ++i) {
                    err_hi += std::fabs(static_cast<double>(grad.pixels[i]) - static_cast<double>(dec_hi->pixels[i]));
                    err_lo += std::fabs(static_cast<double>(grad.pixels[i]) - static_cast<double>(dec_lo->pixels[i]));
                }
                err_hi /= static_cast<double>(grad.pixels.size());
                err_lo /= static_cast<double>(grad.pixels.size());
                TEST_CHECK(err_hi < 2.5);
                TEST_CHECK(err_hi < err_lo);
            }
        }
    }

    // Odd non-multiple-of-8 dimensions (1x1, 17x1, 1x19, 33x47) for both grayscale and RGB.
    const u32 odd_dims[4][2] = {{1, 1}, {17, 1}, {1, 19}, {33, 47}};
    for (const auto& wh : odd_dims) {
        for (u16 ch : {u16{1}, u16{3}}) {
            Image img;
            img.width = wh[0];
            img.height = wh[1];
            img.channels = ch;
            img.sample_type = SampleType::U8;
            img.pixels = Array<u8>(static_cast<usize>(wh[0]) * wh[1] * ch, u8{120});
            auto enc = encode_jpeg(img, JpegEncodeOptions{.quality = 92});
            TEST_CHECK(enc.is_ok());
            if (enc) {
                auto dec = decode_jpeg(*enc);
                TEST_CHECK(dec.is_ok() && dec->width == wh[0] && dec->height == wh[1] && dec->channels == ch);
                if (dec) {
                    for (usize i = 0; i < dec->pixels.size(); ++i) {
                        const int diff = static_cast<int>(dec->pixels[i]) - 120;
                        TEST_CHECK(diff >= -3 && diff <= 3);
                    }
                }
            }
        }
    }

    // Synthetic JPEG with DRI (restart interval = 1 MCU) and RST0/RST1 markers + byte stuffing.
    {
        Image two_mcu;
        two_mcu.width = 16;
        two_mcu.height = 8;
        two_mcu.channels = 1;
        two_mcu.sample_type = SampleType::U8;
        two_mcu.pixels = Array<u8>(16 * 8, u8{0});
        for (u32 r = 0; r < 8; ++r) {
            for (u32 c = 0; c < 16; ++c) {
                two_mcu.pixels[r * 16u + c] = (c < 8u) ? u8{64} : u8{192};
            }
        }
        // Encode each 8x8 half independently so DC prediction starts from 0 in each, then splice with DRI=1 + RST0!
        Image left;
        left.width = 8;
        left.height = 8;
        left.channels = 1;
        left.sample_type = SampleType::U8;
        left.pixels = Array<u8>(64, u8{64});

        Image right;
        right.width = 8;
        right.height = 8;
        right.channels = 1;
        right.sample_type = SampleType::U8;
        right.pixels = Array<u8>(64, u8{192});
        auto j_left = encode_jpeg(left, JpegEncodeOptions{.quality = 95});
        auto j_right = encode_jpeg(right, JpegEncodeOptions{.quality = 95});
        TEST_CHECK(j_left.is_ok() && j_right.is_ok());
        if (j_left && j_right) {
            // Find SOS (FF DA) and EOI (FF D9) in both streams.
            auto find_sos_end = [](ArrayRef<u8> b) -> usize {
                for (usize i = 2; i + 4 < b.size(); ++i) {
                    if (b[i] == 0xFFu && b[i + 1] == 0xDAu) {
                        const usize slen = (static_cast<usize>(b[i + 2]) << 8) | b[i + 3];
                        return i + 2 + slen;
                    }
                }
                return 0;
            };
            const usize sos_end_l = find_sos_end(*j_left);
            const usize sos_end_r = find_sos_end(*j_right);
            TEST_CHECK(sos_end_l > 0 && sos_end_r > 0);

            // Patch SOF0 width from 8 -> 16 and insert DRI (FF DD 00 04 00 01) right before SOS.
            Array<u8> rst_jpeg;
            const usize sos_marker_pos = sos_end_l - 10u; // SOS for 1ch has segLen=8 + 2 marker bytes = 10
            rst_jpeg.append(j_left->slice(0, sos_marker_pos));
            // Patch width in SOF0
            for (usize i = 2; i + 9 < rst_jpeg.size(); ++i) {
                if (rst_jpeg[i] == 0xFFu && rst_jpeg[i + 1] == 0xC0u) {
                    rst_jpeg[i + 7] = 0x00u;
                    rst_jpeg[i + 8] = 16u;
                    break;
                }
            }
            const u8 dri_seg[6] = {0xFFu, 0xDDu, 0x00u, 0x04u, 0x00u, 0x01u};
            rst_jpeg.append(ArrayRef<u8>(dri_seg, 6));
            rst_jpeg.append(j_left->slice(sos_marker_pos, j_left->size() - 2u - sos_marker_pos));
            rst_jpeg.push_back(0xFFu);
            rst_jpeg.push_back(0xD0u); // RST0 between MCU 0 and MCU 1
            rst_jpeg.append(j_right->slice(sos_end_r)); // includes MCU 1 scan + FF D9

            auto dec_rst = decode_jpeg(rst_jpeg);
            TEST_CHECK(dec_rst.is_ok() && dec_rst->width == 16u && dec_rst->height == 8u);
            if (dec_rst) {
                TEST_CHECK(std::abs(static_cast<int>(dec_rst->pixels[0]) - 64) <= 2);
                TEST_CHECK(std::abs(static_cast<int>(dec_rst->pixels[15]) - 192) <= 2);
            }
        }
    }
}

// ============================================================================
// 3. BMP Tests
// ============================================================================

void test_bmp() {
    for (u16 ch : {u16{1}, u16{3}, u16{4}}) {
        for (u32 w : {1u, 3u, 5u, 14u}) {
            Image src = make_test_imagefile(w, 7, ch, SampleType::U8);
            auto enc = encode_bmp(src);
            TEST_CHECK(enc.is_ok());
            if (!enc) continue;

            TEST_CHECK(sniff_image_format(*enc) == ImageFormat::Bmp);
            auto info = peek_bmp(*enc);
            TEST_CHECK(info.is_ok() && info->width == w && info->height == 7u && info->channels == ch);

            auto dec = decode_bmp(*enc);
            TEST_CHECK(dec.is_ok() && dec->pixels == src.pixels);
        }
    }

    // BI_RLE8 compressed 8-bit colormapped BMP (4x2).
    {
        Array<u8> bmp;
        bmp.resize(54 + 2 * 4, u8{0}); // Header (14) + InfoHeader (40) + 2 palette entries (8)
        bmp[0] = 'B';
        bmp[1] = 'M';
        bmp[14] = 40; // BITMAPINFOHEADER
        bmp[18] = 4;  // width = 4
        bmp[22] = 2;  // height = 2 (bottom-up)
        bmp[26] = 1;  // planes = 1
        bmp[28] = 8;  // bpp = 8
        bmp[30] = 1;  // compression = BI_RLE8
        bmp[46] = 2;  // clrUsed = 2
        // Palette entry 0: BGR = (10, 20, 30) -> RGB (30, 20, 10)
        bmp[54] = 10;
        bmp[55] = 20;
        bmp[56] = 30;
        // Palette entry 1: BGR = (200, 100, 50) -> RGB (50, 100, 200)
        bmp[58] = 200;
        bmp[59] = 100;
        bmp[60] = 50;

        const u32 data_off = static_cast<u32>(bmp.size());
        bmp[10] = static_cast<u8>(data_off);
        // Bottom row (r=1): run of 4 pixels of index 1, EOL (00 00)
        // Top row (r=0): absolute mode (00 04) [0, 1, 0, 1], EOB (00 01)
        const u8 rle_stream[] = {
            4, 1, 0, 0,             // row 1 (bottom)
            0, 4, 0, 1, 0, 1, 0, 1, // row 0 (top) + EOB
        };
        bmp.append(ArrayRef<u8>(rle_stream, sizeof(rle_stream)));
        const u32 file_sz = static_cast<u32>(bmp.size());
        std::memcpy(bmp.data() + 2, &file_sz, 4);

        auto dec = decode_bmp(bmp);
        TEST_CHECK(dec.is_ok() && dec->width == 4u && dec->height == 2u && dec->channels == 3u);
        if (dec) {
            // Top-left (0,0) is index 0 -> RGB(30, 20, 10); (0,1) is index 1 -> RGB(50, 100, 200)
            TEST_CHECK(dec->pixels[0] == 30u && dec->pixels[1] == 20u && dec->pixels[2] == 10u);
            TEST_CHECK(dec->pixels[3] == 50u && dec->pixels[4] == 100u && dec->pixels[5] == 200u);
            // Bottom-left (1,0) is index 1 -> RGB(50, 100, 200)
            TEST_CHECK(dec->pixels[12] == 50u && dec->pixels[13] == 100u && dec->pixels[14] == 200u);
        }
    }
}

// ============================================================================
// 4. TGA Tests
// ============================================================================

void test_tga() {
    for (u16 ch : {u16{1}, u16{3}, u16{4}}) {
        Image src = make_test_imagefile(11, 9, ch, SampleType::U8);
        auto enc = encode_tga(src);
        TEST_CHECK(enc.is_ok());
        if (!enc) continue;

        TEST_CHECK(sniff_image_format(*enc) == ImageFormat::Tga);
        auto info = peek_tga(*enc);
        TEST_CHECK(info.is_ok() && info->width == 11u && info->height == 9u && info->channels == ch);

        auto dec = decode_tga(*enc);
        TEST_CHECK(dec.is_ok() && dec->pixels == src.pixels);
    }

    // RLE TrueColor TGA (imageType = 10, 24-bit, top-left origin, 4x1).
    {
        Array<u8> tga(18, u8{0});
        tga[2] = 10;   // RLE TrueColor
        tga[12] = 4;   // width = 4
        tga[14] = 1;   // height = 1
        tga[16] = 24;  // 24 bpp
        tga[17] = 0x20; // top-left origin
        // Run packet of 3 pixels BGR=(1,2,3) -> 0x82, 1, 2, 3; Raw packet of 1 pixel BGR=(9,8,7) -> 0x00, 9, 8, 7
        const u8 pkt[] = {0x82u, 1u, 2u, 3u, 0x00u, 9u, 8u, 7u};
        tga.append(ArrayRef<u8>(pkt, sizeof(pkt)));

        auto dec = decode_tga(tga);
        TEST_CHECK(dec.is_ok() && dec->width == 4u && dec->height == 1u && dec->channels == 3u);
        if (dec) {
            TEST_CHECK(dec->pixels[0] == 3u && dec->pixels[1] == 2u && dec->pixels[2] == 1u);
            TEST_CHECK(dec->pixels[9] == 7u && dec->pixels[10] == 8u && dec->pixels[11] == 9u);
        }
    }

    // Colormapped TGA (imageType = 1, 8-bit indices into 24-bit BGR color map).
    {
        Array<u8> tga(18, u8{0});
        tga[1] = 1;    // colorMapType = 1
        tga[2] = 1;    // uncompressed color-mapped
        tga[5] = 2;    // cmap length = 2
        tga[7] = 24;   // cmap entry size = 24-bit
        tga[12] = 2;   // width = 2
        tga[14] = 1;   // height = 1
        tga[16] = 8;   // 8-bit index
        tga[17] = 0x20; // top-left
        const u8 cmap_and_idx[] = {
            30u, 20u, 10u,   // entry 0: BGR -> RGB(10, 20, 30)
            60u, 50u, 40u,   // entry 1: BGR -> RGB(40, 50, 60)
            1u,  0u,         // indices: [1, 0]
        };
        tga.append(ArrayRef<u8>(cmap_and_idx, sizeof(cmap_and_idx)));
        auto dec = decode_tga(tga);
        TEST_CHECK(dec.is_ok() && dec->channels == 3u);
        if (dec) {
            TEST_CHECK(dec->pixels[0] == 40u && dec->pixels[1] == 50u && dec->pixels[2] == 60u);
            TEST_CHECK(dec->pixels[3] == 10u && dec->pixels[4] == 20u && dec->pixels[5] == 30u);
        }
    }
}

// ============================================================================
// 5. PNM Tests (P1..P6)
// ============================================================================

void test_pnm() {
    // Binary PGM/PPM 8-bit and 16-bit round-trips.
    for (SampleType st : {SampleType::U8, SampleType::U16}) {
        for (u16 ch : {u16{1}, u16{3}}) {
            Image src = make_test_imagefile(15, 7, ch, st);
            auto enc = encode_pnm(src);
            TEST_CHECK(enc.is_ok());
            if (!enc) continue;

            TEST_CHECK(sniff_image_format(*enc) == ImageFormat::Pnm);
            auto info = peek_pnm(*enc);
            TEST_CHECK(info.is_ok() && info->width == 15u && info->height == 7u && info->channels == ch &&
                       info->sample_type == st);

            auto dec = decode_pnm(*enc);
            TEST_CHECK(dec.is_ok() && dec->pixels == src.pixels);
        }
    }

    // PBM (P4 binary) round-trip (width=11 exercises packed bit row padding).
    {
        Image bw;
        bw.width = 11;
        bw.height = 3;
        bw.channels = 1;
        bw.sample_type = SampleType::U8;
        bw.pixels = Array<u8>(33, u8{0});
        for (usize i = 0; i < 33; ++i) {
            bw.pixels[i] = ((i % 3u) == 0u) ? u8{255} : u8{0};
        }
        auto enc = encode_pnm(bw, PnmEncodeOptions{.subformat = PnmSubformat::Pbm});
        TEST_CHECK(enc.is_ok());
        if (enc) {
            auto dec = decode_pnm(*enc);
            TEST_CHECK(dec.is_ok() && dec->pixels == bw.pixels);
        }
    }

    // ASCII P1, P2 (with '#' comments), and P3.
    {
        const char* p1 = "P1\n# PBM comment\n3 2\n1 0 1\n0 1 0\n";
        auto d1 = decode_pnm(ArrayRef<u8>(reinterpret_cast<const u8*>(p1), std::strlen(p1)));
        TEST_CHECK(d1.is_ok() && d1->width == 3u && d1->height == 2u);
        if (d1) {
            TEST_CHECK(d1->pixels[0] == 0u && d1->pixels[1] == 255u && d1->pixels[2] == 0u);
            TEST_CHECK(d1->pixels[3] == 255u && d1->pixels[4] == 0u && d1->pixels[5] == 255u);
        }

        const char* p2 = "P2\n# Comment 1\n2 # inline comment\n2\n10\n0 5\n10 2\n";
        auto d2 = decode_pnm(ArrayRef<u8>(reinterpret_cast<const u8*>(p2), std::strlen(p2)));
        TEST_CHECK(d2.is_ok() && d2->width == 2u && d2->height == 2u);
        if (d2) {
            TEST_CHECK(d2->pixels[0] == 0u && d2->pixels[1] == 128u && d2->pixels[2] == 255u && d2->pixels[3] == 51u);
        }

        const char* p3 = "P3\n2 1\n255\n12 34 56  78 90 250\n";
        auto d3 = decode_pnm(ArrayRef<u8>(reinterpret_cast<const u8*>(p3), std::strlen(p3)));
        TEST_CHECK(d3.is_ok() && d3->width == 2u && d3->height == 1u && d3->channels == 3u);
        if (d3) {
            TEST_CHECK(d3->pixels[0] == 12u && d3->pixels[2] == 56u && d3->pixels[3] == 78u && d3->pixels[5] == 250u);
        }
    }

    // Regression check: binary P5 whose first pixel byte is '#' (0x23) or ' ' (0x20) must NOT be eaten as whitespace!
    {
        const u8 p5_special[] = {'P', '5', '\n', '2', ' ', '1', '\n', '2', '5', '5', '\n', 0x20u, 0x23u};
        auto dec = decode_pnm(ArrayRef<u8>(p5_special, sizeof(p5_special)));
        TEST_CHECK(dec.is_ok() && dec->pixels.size() == 2u);
        if (dec) {
            TEST_CHECK(dec->pixels[0] == 0x20u && dec->pixels[1] == 0x23u);
        }
    }
}

// ============================================================================
// 6. GIF Tests (Sequential & 4-Pass Interlaced)
// ============================================================================

Array<u8> build_test_gif(u16 w, u16 h, bool interlaced, ArrayRef<u8> stream_indices) {
    Array<u8> gif;
    const u8 hdr[13] = {
        'G', 'I', 'F', '8', '9', 'a',
        static_cast<u8>(w & 0xFFu),
        static_cast<u8>((w >> 8) & 0xFFu),
        static_cast<u8>(h & 0xFFu),
        static_cast<u8>((h >> 8) & 0xFFu),
        0x81u, // GCT present, 2^(1+1) = 4 entries
        0u,
        0u,
    };
    gif.append(ArrayRef<u8>(hdr, 13));
    const u8 gct[12] = {
        0,   0,   0,   // 0: black
        255, 0,   0,   // 1: red
        0,   255, 0,   // 2: green
        0,   0,   255, // 3: blue
    };
    gif.append(ArrayRef<u8>(gct, 12));

    // Image Descriptor (0x2C)
    const u8 desc[10] = {
        0x2Cu, 0u, 0u, 0u, 0u,
        static_cast<u8>(w & 0xFFu),
        static_cast<u8>((w >> 8) & 0xFFu),
        static_cast<u8>(h & 0xFFu),
        static_cast<u8>((h >> 8) & 0xFFu),
        static_cast<u8>(interlaced ? 0x40u : 0x00u),
    };
    gif.append(ArrayRef<u8>(desc, 10));

    // LZW with minCodeSize = 2 -> ClearCode = 4, EoiCode = 5, initial codeSize = 3.
    // Emit ClearCode (4) before every literal pixel so codeSize stays 3 bits for arbitrary length!
    gif.push_back(2u); // minCodeSize = 2
    Array<u8> lzw_bytes;
    u32 bit_buf = 0;
    int bit_cnt = 0;
    auto put3 = [&](u32 code) {
        bit_buf |= (code & 7u) << bit_cnt;
        bit_cnt += 3;
        while (bit_cnt >= 8) {
            lzw_bytes.push_back(static_cast<u8>(bit_buf & 0xFFu));
            bit_buf >>= 8;
            bit_cnt -= 8;
        }
    };
    for (usize i = 0; i < stream_indices.size(); ++i) {
        put3(4u); // ClearCode resets nextCode=6, codeSize=3
        put3(stream_indices[i] & 3u);
    }
    put3(5u); // EOI
    if (bit_cnt > 0) {
        lzw_bytes.push_back(static_cast<u8>(bit_buf & 0xFFu));
    }

    usize pos = 0;
    while (pos < lzw_bytes.size()) {
        const usize chunk = (lzw_bytes.size() - pos > 255u) ? 255u : (lzw_bytes.size() - pos);
        gif.push_back(static_cast<u8>(chunk));
        gif.append(lzw_bytes.slice(pos, chunk));
        pos += chunk;
    }
    gif.push_back(0u);    // Block terminator
    gif.push_back(0x3Bu); // GIF Trailer
    return gif;
}

void test_gif() {
    // Sequential 4x3 GIF
    {
        u8 idx[12]{};
        for (u8 i = 0; i < 12; ++i) idx[i] = static_cast<u8>(i & 3u);
        Array<u8> gif = build_test_gif(4, 3, false, ArrayRef<u8>(idx, 12));

        TEST_CHECK(sniff_image_format(gif) == ImageFormat::Gif);
        auto info = peek_gif(gif);
        TEST_CHECK(info.is_ok() && info->width == 4u && info->height == 3u && info->has_palette);

        auto dec = decode_gif(gif);
        TEST_CHECK(dec.is_ok());
        if (dec) {
            TEST_CHECK(dec->channels == 1u && dec->palette_bits == 8u && dec->palette_entry_count() == 4u);
            TEST_CHECK(dec->pixels == ArrayRef<u8>(idx, 12));

            auto rgb = decode_gif(gif, ImageDecodeOptions{.expand_indexed = true});
            TEST_CHECK(rgb.is_ok() && rgb->channels == 3u);
            if (rgb) {
                // Pixel 1 has index 1 -> Red (255, 0, 0)
                TEST_CHECK(rgb->pixels[3] == 255u && rgb->pixels[4] == 0u && rgb->pixels[5] == 0u);
                // Pixel 2 has index 2 -> Green (0, 255, 0)
                TEST_CHECK(rgb->pixels[6] == 0u && rgb->pixels[7] == 255u && rgb->pixels[8] == 0u);
            }
        }
    }

    // 4-Pass Interlaced 2x9 GIF (rows 0..8 exercise all 4 GIF interlace passes: 0,8 | 4 | 2,6 | 1,3,5,7).
    {
        const u32 W = 2, H = 9;
        const u32 interlace_row_order[9] = {0, 8, 4, 2, 6, 1, 3, 5, 7};
        u8 stream[W * H]{};
        for (u32 s = 0; s < 9; ++s) {
            const u32 r = interlace_row_order[s];
            stream[s * 2u + 0] = static_cast<u8>(r & 3u);
            stream[s * 2u + 1] = static_cast<u8>((r + 1u) & 3u);
        }
        Array<u8> gif = build_test_gif(W, H, true, ArrayRef<u8>(stream, sizeof(stream)));
        auto dec = decode_gif(gif);
        TEST_CHECK(dec.is_ok() && dec->width == W && dec->height == H);
        if (dec) {
            for (u32 r = 0; r < H; ++r) {
                TEST_CHECK(dec->pixels[r * 2u + 0] == static_cast<u8>(r & 3u));
                TEST_CHECK(dec->pixels[r * 2u + 1] == static_cast<u8>((r + 1u) & 3u));
            }
        }
    }
}

// ============================================================================
// 7. TIFF Fixture & Comprehensive Codec Tests
// ============================================================================

Array<u8> load_fixture(const char* name) {
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/%s", KEEL_IMAGEFILE_FIXTURES_DIR, name);
    auto r = fs_read_file(StringRef(path, std::strlen(path)));
    TEST_CHECK(r.is_ok());
    return r.is_ok() ? keel::move(*r) : Array<u8>{};
}

void test_tiff_fixtures() {
    // 1. Gray8 baseline + BigTIFF + all compression variants
    Array<u8> g8_bytes = load_fixture("gray8.tif");
    auto g8 = decode_tiff(g8_bytes);
    TEST_CHECK(g8.is_ok());
    if (g8) {
        TEST_CHECK(g8->width == 4u && g8->height == 4u && g8->channels == 1u && g8->sample_type == SampleType::U8);
        TEST_CHECK(g8->pixels[0 * 4 + 0] == 1u);
        TEST_CHECK(g8->pixels[1 * 4 + 0] == 2u);
        TEST_CHECK(g8->pixels[0 * 4 + 1] == 5u);
        TEST_CHECK(g8->pixels[3 * 4 + 3] == 16u);

        Array<u8> big_g8_bytes = load_fixture("bigtiff_gray8.tif");
        auto big_g8_info = peek_tiff(big_g8_bytes);
        TEST_CHECK(big_g8_info.is_ok() && big_g8_info->width == 4u && big_g8_info->height == 4u &&
                   big_g8_info->channels == 1u);
        auto big_g8 = decode_tiff(big_g8_bytes);
        TEST_CHECK(big_g8.is_ok() && big_g8->pixels == g8->pixels);
    }

    Array<u8> g8_none_bytes = load_fixture("gray8_none.tif");
    auto g8_ref = decode_tiff(g8_none_bytes);
    TEST_CHECK(g8_ref.is_ok());
    if (g8_ref) {
        for (const char* name : {"gray8_packbits.tif", "gray8_lzw.tif", "gray8_deflate.tif"}) {
            Array<u8> fb = load_fixture(name);
            auto info = peek_tiff(fb);
            TEST_CHECK(info.is_ok() && info->width == g8_ref->width && info->height == g8_ref->height &&
                       info->channels == 1u);
            auto dec = decode_tiff(fb);
            TEST_CHECK(dec.is_ok() && dec->pixels == g8_ref->pixels);
        }
    }

    // 2. RGB8 baseline + BigTIFF + all compression variants
    Array<u8> rgb8_bytes = load_fixture("rgb8.tif");
    auto rgb8 = decode_tiff(rgb8_bytes);
    TEST_CHECK(rgb8.is_ok());
    if (rgb8) {
        TEST_CHECK(rgb8->width == 4u && rgb8->height == 4u && rgb8->channels == 3u &&
                   rgb8->sample_type == SampleType::U8);
        TEST_CHECK(rgb8->pixels[0] == 0u && rgb8->pixels[1] == 16u && rgb8->pixels[2] == 32u);
        TEST_CHECK(rgb8->pixels[15 * 3 + 0] == 15u && rgb8->pixels[15 * 3 + 1] == 31u &&
                   rgb8->pixels[15 * 3 + 2] == 47u);

        Array<u8> big_rgb8_bytes = load_fixture("bigtiff_rgb8.tif");
        auto big_rgb8_info = peek_tiff(big_rgb8_bytes);
        TEST_CHECK(big_rgb8_info.is_ok() && big_rgb8_info->width == 4u && big_rgb8_info->height == 4u &&
                   big_rgb8_info->channels == 3u);
        auto big_rgb8 = decode_tiff(big_rgb8_bytes);
        TEST_CHECK(big_rgb8.is_ok() && big_rgb8->pixels == rgb8->pixels);
    }

    Array<u8> rgb8_none_bytes = load_fixture("rgb8_none.tif");
    auto rgb8_ref = decode_tiff(rgb8_none_bytes);
    TEST_CHECK(rgb8_ref.is_ok());
    if (rgb8_ref) {
        for (const char* name : {"rgb8_packbits.tif", "rgb8_lzw.tif", "rgb8_deflate.tif"}) {
            Array<u8> fb = load_fixture(name);
            auto info = peek_tiff(fb);
            TEST_CHECK(info.is_ok() && info->width == rgb8_ref->width && info->height == rgb8_ref->height &&
                       info->channels == 3u);
            auto dec = decode_tiff(fb);
            TEST_CHECK(dec.is_ok() && dec->pixels == rgb8_ref->pixels);
        }
    }

    // 3. Gray16 baseline + all compression variants
    Array<u8> g16_bytes = load_fixture("gray16.tif");
    auto g16 = decode_tiff(g16_bytes);
    TEST_CHECK(g16.is_ok());
    if (g16) {
        TEST_CHECK(g16->width == 4u && g16->height == 4u && g16->channels == 1u && g16->sample_type == SampleType::U16);
        u16 p00 = 0, p33 = 0;
        std::memcpy(&p00, g16->pixels.data() + 0, 2);
        std::memcpy(&p33, g16->pixels.data() + 15 * 2, 2);
        TEST_CHECK(p00 == 1000u && p33 == 1015u);
    }

    Array<u8> g16_none_bytes = load_fixture("gray16_none.tif");
    auto g16_ref = decode_tiff(g16_none_bytes);
    TEST_CHECK(g16_ref.is_ok() && g16_ref->sample_type == SampleType::U16);
    if (g16_ref) {
        for (const char* name : {"gray16_packbits.tif", "gray16_lzw.tif", "gray16_deflate.tif"}) {
            Array<u8> fb = load_fixture(name);
            auto info = peek_tiff(fb);
            TEST_CHECK(info.is_ok() && info->bits_per_sample == 16u && info->sample_type == SampleType::U16);
            auto dec = decode_tiff(fb);
            TEST_CHECK(dec.is_ok() && dec->pixels == g16_ref->pixels);
        }
    }

    // 4. Multi-page TIFF (0-based pages 0, 1, 2; page 3 -> OutOfRange)
    {
        Array<u8> mp = load_fixture("multipage_gray8.tif");
        auto n_pages = tiff_page_count(mp);
        TEST_CHECK(n_pages.is_ok() && *n_pages == 3u);
        auto info = peek_tiff(mp);
        TEST_CHECK(info.is_ok() && info->page_count == 3u);

        auto p0 = decode_tiff(mp, ImageDecodeOptions{.page = 0});
        auto p1 = decode_tiff(mp, ImageDecodeOptions{.page = 1});
        auto p2 = decode_tiff(mp, ImageDecodeOptions{.page = 2});
        auto p3 = decode_tiff(mp, ImageDecodeOptions{.page = 3});
        TEST_CHECK(p0.is_ok() && p0->pixels[0] == 1u && p0->pixels[15] == 16u);
        TEST_CHECK(p1.is_ok() && p1->pixels[0] == 100u && p1->pixels[15] == 115u);
        TEST_CHECK(p2.is_ok() && p2->pixels[0] == 200u && p2->pixels[15] == 215u);
        TEST_CHECK(p3.is_err() && p3.error().code == ErrorCode::OutOfRange);
    }

    // 5. Palette-indexed TIFF (Photometric == 3, 16-bit ColorMap)
    {
        Array<u8> pal_tif = load_fixture("palette_indexed.tif");
        auto info = peek_tiff(pal_tif);
        TEST_CHECK(info.is_ok() && info->has_palette && info->channels == 1u);

        auto dec = decode_tiff(pal_tif);
        TEST_CHECK(dec.is_ok());
        if (dec) {
            TEST_CHECK(dec->channels == 1u && dec->sample_type == SampleType::U8);
            TEST_CHECK(dec->has_palette() && dec->palette_bits == 16u && dec->palette_entry_count() == 256u);
            // Entry 1 = Red (65535, 0, 0); Entry 2 = Green; Entry 3 = Blue; Entry 4 = White
            TEST_CHECK(dec->palette[1 * 3 + 0] == 65535u && dec->palette[1 * 3 + 1] == 0u);
            TEST_CHECK(dec->palette[2 * 3 + 1] == 65535u);
            TEST_CHECK(dec->palette[3 * 3 + 2] == 65535u);
            TEST_CHECK(dec->palette[4 * 3 + 0] == 65535u && dec->palette[4 * 3 + 2] == 65535u);

            auto exp = expand_palette(*dec);
            TEST_CHECK(exp.is_ok() && exp->channels == 3u && exp->sample_type == SampleType::U16);
            auto exp2 = decode_tiff(pal_tif, ImageDecodeOptions{.expand_indexed = true});
            TEST_CHECK(exp2.is_ok() && exp2->pixels == exp->pixels);
        }
    }
}

void test_tiff_roundtrips() {
    const SampleType all_types[8] = {
        SampleType::U8, SampleType::U16, SampleType::U32, SampleType::I8,
        SampleType::I16, SampleType::I32, SampleType::F32, SampleType::F64,
    };
    const TiffCompression all_comps[4] = {
        TiffCompression::None,
        TiffCompression::PackBits,
        TiffCompression::Lzw,
        TiffCompression::Deflate,
    };

    // Every SampleType x channels (1, 3, 4) x compression (None, PackBits, Lzw, Deflate) in Classic & BigTIFF.
    for (SampleType st : all_types) {
        for (u16 ch : {u16{1}, u16{3}, u16{4}}) {
            Image src = make_test_imagefile(9, 7, ch, st);
            for (TiffCompression comp : all_comps) {
                for (bool big : {false, true}) {
                    auto enc = encode_tiff(src, TiffEncodeOptions{.compression = comp, .bigtiff = big});
                    TEST_CHECK(enc.is_ok());
                    if (!enc) continue;

                    TEST_CHECK((*enc)[2] == (big ? 43u : 42u));
                    auto dec = decode_tiff(*enc);
                    TEST_CHECK(dec.is_ok());
                    if (dec) {
                        TEST_CHECK(dec->width == 9u && dec->height == 7u && dec->channels == ch);
                        TEST_CHECK(dec->sample_type == st);
                        TEST_CHECK(dec->pixels == src.pixels);
                    }
                }
            }
        }
    }

    // Large 128x128x3 LZW round-trip crossing the 4096-entry ClearCode table reset.
    {
        Image big_img = make_test_imagefile(128, 128, 3, SampleType::U8);
        auto enc = encode_tiff(big_img, TiffEncodeOptions{.compression = TiffCompression::Lzw});
        TEST_CHECK(enc.is_ok());
        if (enc) {
            auto dec = decode_tiff(*enc);
            TEST_CHECK(dec.is_ok() && dec->pixels == big_img.pixels);
        }
    }

    // Multi-page append: Page 0 Classic -> Page 1 Classic -> Page 2 upgraded to BigTIFF!
    {
        Image p0 = make_test_imagefile(6, 5, 1, SampleType::U8);
        Image p1 = make_test_imagefile(8, 4, 3, SampleType::U16);
        Image p2 = make_test_imagefile(5, 7, 4, SampleType::F32);

        auto t0 = encode_tiff(p0, TiffEncodeOptions{.compression = TiffCompression::None});
        TEST_CHECK(t0.is_ok() && (*t0)[2] == 42u);

        auto t1 = encode_tiff(p1, TiffEncodeOptions{.compression = TiffCompression::Lzw}, *t0);
        TEST_CHECK(t1.is_ok() && (*t1)[2] == 42u);
        auto cnt2 = tiff_page_count(*t1);
        TEST_CHECK(cnt2.is_ok() && *cnt2 == 2u);

        // Append page 2 with bigtiff=true -> upgrades existing 2 Classic pages to BigTIFF seamlessly!
        auto t2 = encode_tiff(p2, TiffEncodeOptions{.compression = TiffCompression::Deflate, .bigtiff = true}, *t1);
        TEST_CHECK(t2.is_ok() && (*t2)[2] == 43u);
        auto cnt3 = tiff_page_count(*t2);
        TEST_CHECK(cnt3.is_ok() && *cnt3 == 3u);

        auto d0 = decode_tiff(*t2, ImageDecodeOptions{.page = 0});
        auto d1 = decode_tiff(*t2, ImageDecodeOptions{.page = 1});
        auto d2 = decode_tiff(*t2, ImageDecodeOptions{.page = 2});
        TEST_CHECK(d0.is_ok() && d0->pixels == p0.pixels);
        TEST_CHECK(d1.is_ok() && d1->pixels == p1.pixels && d1->sample_type == SampleType::U16);
        TEST_CHECK(d2.is_ok() && d2->pixels == p2.pixels && d2->sample_type == SampleType::F32);
    }

    // Synthetic Big-Endian ("MM") TIFF with Photometric=0 (WhiteIsZero) 16-bit grayscale.
    {
        // Header (8): 'M','M', 0,42, 0,0,0,16 (IFD at offset 16)
        // Pixels at offset 8 (4 bytes = 2x1 U16 BE): [0x0000, 0xFFF5] -> inverted WhiteIsZero: [65535, 10]
        const u8 mm_tiff[] = {
            'M',  'M',  0x00, 0x2A, 0x00, 0x00, 0x00, 0x10, // Header
            0x00, 0x00, 0xFF, 0xF5, 0x00, 0x00, 0x00, 0x00, // Pixel strip (4B) + 4B pad
            0x00, 0x07,                                     // 7 IFD entries
            0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, // 256 Width = 2
            0x01, 0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, // 257 Height = 1
            0x01, 0x02, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x10, 0x00, 0x00, // 258 BitsPerSample = 16
            0x01, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, // 259 Compression = 1
            0x01, 0x06, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, // 262 Photometric = 0 (WhiteIsZero)
            0x01, 0x11, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, // 273 StripOffsets = 8
            0x01, 0x17, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, // 279 StripByteCounts = 4
            0x00, 0x00, 0x00, 0x00,                                                 // next IFD = 0
        };
        auto dec = decode_tiff(ArrayRef<u8>(mm_tiff, sizeof(mm_tiff)));
        TEST_CHECK(dec.is_ok() && dec->sample_type == SampleType::U16);
        if (dec) {
            u16 v0 = 0, v1 = 0;
            std::memcpy(&v0, dec->pixels.data() + 0, 2);
            std::memcpy(&v1, dec->pixels.data() + 2, 2);
            TEST_CHECK(v0 == 65535u && v1 == 10u);
        }
    }

    // Synthetic Tiled + PlanarConfiguration=2 RGB TIFF (3x2 image in 2x2 tiles -> 2x1 tiles per plane = 6 tiles).
    {
        Array<u8> tiled;
        auto push16 = [&](u16 v) {
            tiled.push_back(static_cast<u8>(v & 0xFFu));
            tiled.push_back(static_cast<u8>((v >> 8) & 0xFFu));
        };
        auto push32 = [&](u32 v) {
            tiled.push_back(static_cast<u8>(v & 0xFFu));
            tiled.push_back(static_cast<u8>((v >> 8) & 0xFFu));
            tiled.push_back(static_cast<u8>((v >> 16) & 0xFFu));
            tiled.push_back(static_cast<u8>((v >> 24) & 0xFFu));
        };

        // Header: II, 42, IFD offset patched later
        tiled.push_back('I');
        tiled.push_back('I');
        push16(42u);
        push32(0u);

        // 6 tiles (each 2x2 = 4 bytes), starting at offset 8:
        // Plane 0 (R): tile(0,0) = [10, 11, 20, 21], tile(1,0) = [12, 99, 22, 99] (col 2 clipped at W=3)
        // Plane 1 (G): tile(0,0) = [30, 31, 40, 41], tile(1,0) = [32, 99, 42, 99]
        // Plane 2 (B): tile(0,0) = [50, 51, 60, 61], tile(1,0) = [52, 99, 62, 99]
        const u8 tile_payload[24] = {
            10, 11, 20, 21, 12, 99, 22, 99, // R
            30, 31, 40, 41, 32, 99, 42, 99, // G
            50, 51, 60, 61, 52, 99, 62, 99, // B
        };
        const u32 tiles_base = static_cast<u32>(tiled.size()); // 8
        tiled.append(ArrayRef<u8>(tile_payload, 24));

        const u32 bps_off = static_cast<u32>(tiled.size());
        push16(8u);
        push16(8u);
        push16(8u);

        const u32 toff_arr = static_cast<u32>(tiled.size());
        for (u32 i = 0; i < 6u; ++i) push32(tiles_base + i * 4u);

        const u32 tcnt_arr = static_cast<u32>(tiled.size());
        for (u32 i = 0; i < 6u; ++i) push32(4u);

        const u32 ifd_off = static_cast<u32>(tiled.size());
        std::memcpy(tiled.data() + 4, &ifd_off, 4);

        auto add_entry = [&](u16 tag, u16 type, u32 cnt, u32 val) {
            push16(tag);
            push16(type);
            push32(cnt);
            push32(val);
        };
        push16(11u);
        add_entry(256, 4, 1, 3);        // Width = 3
        add_entry(257, 4, 1, 2);        // Height = 2
        add_entry(258, 3, 3, bps_off);  // BitsPerSample = [8,8,8]
        add_entry(259, 3, 1, 1);        // Compression = None
        add_entry(262, 3, 1, 2);        // Photometric = RGB
        add_entry(277, 3, 1, 3);        // SamplesPerPixel = 3
        add_entry(284, 3, 1, 2);        // PlanarConfiguration = 2 (Planar)
        add_entry(322, 4, 1, 2);        // TileWidth = 2
        add_entry(323, 4, 1, 2);        // TileLength = 2
        add_entry(324, 4, 6, toff_arr); // TileOffsets
        add_entry(325, 4, 6, tcnt_arr); // TileByteCounts
        push32(0u);

        auto dec = decode_tiff(tiled);
        TEST_CHECK(dec.is_ok() && dec->width == 3u && dec->height == 2u && dec->channels == 3u);
        if (dec) {
            const u8 expected[18] = {
                10, 30, 50, 11, 31, 51, 12, 32, 52, // row 0
                20, 40, 60, 21, 41, 61, 22, 42, 62, // row 1
            };
            TEST_CHECK(dec->pixels == ArrayRef<u8>(expected, 18));
        }
    }
}

// ============================================================================
// 8. File I/O, Sniffing, Robustness & Custom Arena Allocator Routing
// ============================================================================

void test_file_io_and_robustness() {
    // Unified write_image / peek_image_file / read_image round-trips
    Image rgb = make_test_imagefile(12, 8, 3, SampleType::U8);
    const char* exts[] = {"png", "bmp", "tga", "ppm", "tif", "jpg"};
    const ImageFormat expected_fmts[] = {
        ImageFormat::Png, ImageFormat::Bmp,  ImageFormat::Tga,
        ImageFormat::Pnm, ImageFormat::Tiff, ImageFormat::Jpeg,
    };

    for (usize i = 0; i < 6; ++i) {
        char tmp_path[256];
        std::snprintf(tmp_path, sizeof(tmp_path), "/tmp/keel_img_fmt_test_%zu.%s", i, exts[i]);
        StringRef sp(tmp_path, std::strlen(tmp_path));

        auto wr = write_image(sp, rgb);
        TEST_CHECK(wr.is_ok());

        auto pf = peek_image_file(sp);
        TEST_CHECK(pf.is_ok() && pf->format == expected_fmts[i] && pf->width == 12u && pf->height == 8u);

        auto rd = read_image(sp);
        TEST_CHECK(rd.is_ok() && rd->width == 12u && rd->height == 8u && rd->channels == 3u);
        if (rd && expected_fmts[i] != ImageFormat::Jpeg) {
            TEST_CHECK(rd->pixels == rgb.pixels);
        }

        (void)fs_remove(sp);
    }

    // Empty / garbage / truncated / corrupted inputs
    ArrayRef<u8> empty{};
    TEST_CHECK(sniff_image_format(empty) == ImageFormat::Unknown);
    TEST_CHECK(decode_image(empty).error().code == ErrorCode::EndOfFile);
    TEST_CHECK(peek_image(empty).error().code == ErrorCode::EndOfFile);

    const u8 garbage[32] = {0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4};
    TEST_CHECK(decode_image(ArrayRef<u8>(garbage, sizeof(garbage))).error().code == ErrorCode::InvalidData);

    // Corrupted PNG chunk CRC must fail with InvalidData
    auto valid_png = encode_png(rgb);
    TEST_CHECK(valid_png.is_ok());
    if (valid_png) {
        Array<u8> bad_crc = valid_png->clone();
        bad_crc[bad_crc.size() - 15u] ^= 0xFFu; // Flip byte inside IDAT payload/CRC
        TEST_CHECK(decode_png(bad_crc).is_err());

        // Truncated PNG -> EndOfFile
        TEST_CHECK(decode_png(valid_png->slice(0, 14)).error().code == ErrorCode::EndOfFile);
        TEST_CHECK(decode_png(valid_png->slice(0, valid_png->size() - 6u)).error().code == ErrorCode::EndOfFile);

        // Dimension & byte caps -> OutOfRange
        TEST_CHECK(decode_png(*valid_png, ImageDecodeOptions{.max_width = 8}).error().code == ErrorCode::OutOfRange);
        TEST_CHECK(decode_png(*valid_png, ImageDecodeOptions{.max_height = 4}).error().code == ErrorCode::OutOfRange);
        TEST_CHECK(decode_png(*valid_png, ImageDecodeOptions{.max_pixels = 50}).error().code == ErrorCode::OutOfRange);
        TEST_CHECK(decode_png(*valid_png, ImageDecodeOptions{.max_bytes = 100}).error().code == ErrorCode::OutOfRange);
    }

    // Cyclic TIFF IFD pointer protection -> InvalidData (no infinite loop!)
    {
        const u8 cyclic_tiff[24] = {
            'I', 'I', 42, 0, 8, 0, 0, 0, // first IFD = 8
            0,   0,                      // 0 entries
            8,   0,   0,  0,             // next IFD = 8 (self-cycle!)
        };
        TEST_CHECK(tiff_page_count(ArrayRef<u8>(cyclic_tiff, sizeof(cyclic_tiff))).error().code ==
                   ErrorCode::InvalidData);
        TEST_CHECK(decode_tiff(ArrayRef<u8>(cyclic_tiff, sizeof(cyclic_tiff)), ImageDecodeOptions{.page = 5})
                       .error()
                       .code == ErrorCode::InvalidData);
    }

    // Invalid ImageView & unsupported encode combinations
    TEST_CHECK(encode_png(ImageView{}).error().code == ErrorCode::InvalidArgument);
    TEST_CHECK(encode_image(rgb, ImageFormat::Gif).error().code == ErrorCode::NotSupported);
    TEST_CHECK(encode_image(rgb, ImageFormat::Unknown).error().code == ErrorCode::InvalidArgument);
    Image rgba16 = make_test_imagefile(4, 4, 4, SampleType::U16);
    TEST_CHECK(encode_jpeg(rgba16).error().code == ErrorCode::NotSupported);
    TEST_CHECK(encode_bmp(rgba16).error().code == ErrorCode::NotSupported);
    TEST_CHECK(encode_tga(rgba16).error().code == ErrorCode::NotSupported);

    // Custom Arena allocator routing (also verifies conflict-free get_scratch when caller passes Arena::allocator()!)
    {
        Arena arena(4 * 1024 * 1024);
        Allocator* a = arena.allocator();
        const usize used0 = arena.used();

        auto png_b = encode_png(rgb, PngEncodeOptions{.allocator = a});
        TEST_CHECK(png_b.is_ok() && png_b->allocator() == a);
        TEST_CHECK(arena.used() > used0);

        auto png_d = decode_png(*png_b, ImageDecodeOptions{.allocator = a});
        TEST_CHECK(png_d.is_ok() && png_d->pixels.allocator() == a && png_d->pixels == rgb.pixels);

        auto tif_b = encode_tiff(rgb, TiffEncodeOptions{.compression = TiffCompression::Lzw, .allocator = a});
        TEST_CHECK(tif_b.is_ok() && tif_b->allocator() == a);
        auto tif_d = decode_tiff(*tif_b, ImageDecodeOptions{.allocator = a});
        TEST_CHECK(tif_d.is_ok() && tif_d->pixels.allocator() == a && tif_d->pixels == rgb.pixels);

        auto jpg_b = encode_jpeg(rgb, JpegEncodeOptions{.quality = 90, .allocator = a});
        TEST_CHECK(jpg_b.is_ok() && jpg_b->allocator() == a);
        auto jpg_d = decode_jpeg(*jpg_b, ImageDecodeOptions{.allocator = a});
        TEST_CHECK(jpg_d.is_ok() && jpg_d->pixels.allocator() == a);
    }
}

} // namespace

int main() {
    test_png();
    test_jpeg();
    test_bmp();
    test_tga();
    test_pnm();
    test_gif();
    test_tiff_fixtures();
    test_tiff_roundtrips();
    test_file_io_and_robustness();

    if (g_tests_failed != 0) {
        std::fprintf(stderr, "%d test check(s) FAILED (%d passed)\n", g_tests_failed, g_tests_passed);
        return 1;
    }
    std::printf("All %d keel::imagefile test checks passed.\n", g_tests_passed);
    return 0;
}
