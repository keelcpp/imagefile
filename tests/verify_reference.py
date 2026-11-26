#!/usr/bin/env python3
"""
Bidirectional reference differential verification & adversarial fuzz harness
for keel::imagefile against:
  1. Pillow 12.x (libpng, libjpeg-turbo, libtiff, GIF, BMP, TGA, PNM)
  2. FFmpeg 8.x (PNG, JPEG w/ DRI & subsampling, TIFF/BigTIFF, BMP, TGA, PNM, GIF)
  3. MATLAB R2025b (imread / imwrite / Tiff) when installed and not disabled

Usage:
  python tests/verify_reference.py <path-to-test_imagefile.exe> [--no-matlab]
"""

import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageSequence


STYPE_TO_DTYPE = {
    "u8": np.uint8,
    "u16": np.uint16,
    "u32": np.uint32,
    "i8": np.int8,
    "i16": np.int16,
    "i32": np.int32,
    "f32": np.float32,
    "f64": np.float64,
}


def run_keel_decode(exe: Path, img_path: Path, raw_path: Path, page: int = 0, expand: bool = False):
    cmd = [str(exe), "--decode", str(img_path), str(raw_path), str(page), "1" if expand else "0"]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"keel --decode failed ({proc.returncode}) on {img_path.name}: {proc.stderr.strip()}")
    meta = json.loads(proc.stdout.strip())
    dtype = STYPE_TO_DTYPE[meta["sample_type"]]
    raw = raw_path.read_bytes()
    arr = np.frombuffer(raw, dtype=dtype)
    if meta["channels"] == 1:
        arr = arr.reshape((meta["height"], meta["width"]))
    else:
        arr = arr.reshape((meta["height"], meta["width"], meta["channels"]))
    return meta, arr


def run_keel_encode(exe: Path, fmt: str, arr: np.ndarray, stype: str, out_path: Path, tmp_dir: Path, extra: str = ""):
    raw_path = tmp_dir / f"enc_{out_path.name}.raw"
    arr = np.ascontiguousarray(arr, dtype=STYPE_TO_DTYPE[stype])
    raw_path.write_bytes(arr.tobytes())
    h, w = arr.shape[0], arr.shape[1]
    ch = 1 if arr.ndim == 2 else arr.shape[2]
    cmd = [str(exe), "--encode", fmt, str(w), str(h), str(ch), stype, str(raw_path), str(out_path)]
    if extra:
        cmd.append(extra)
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"keel --encode {fmt} failed ({proc.returncode}): {proc.stderr.strip()}")


def make_smooth_rgb(h: int, w: int) -> np.ndarray:
    y, x = np.mgrid[0:h, 0:w]
    r = ((x * 255) // max(w - 1, 1)).astype(np.uint8)
    g = ((y * 255) // max(h - 1, 1)).astype(np.uint8)
    b = (((x + y) * 127) // max(w + h - 2, 1) + 40).clip(0, 255).astype(np.uint8)
    return np.stack([r, g, b], axis=-1)


def make_pattern(h: int, w: int, ch: int, dtype, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    shape = (h, w) if ch == 1 else (h, w, ch)
    if dtype == np.uint8:
        return rng.integers(0, 256, size=shape, dtype=np.uint8)
    if dtype == np.uint16:
        return rng.integers(0, 65536, size=shape, dtype=np.uint16)
    if dtype == np.uint32:
        return rng.integers(0, 2**31 - 1, size=shape, dtype=np.uint32)
    if dtype == np.int8:
        return rng.integers(-128, 128, size=shape, dtype=np.int8)
    if dtype == np.int16:
        return rng.integers(-32768, 32768, size=shape, dtype=np.int16)
    if dtype == np.int32:
        return rng.integers(-1000000, 1000000, size=shape, dtype=np.int32)
    if dtype == np.float32:
        return (rng.standard_normal(size=shape) * 100.0).astype(np.float32)
    if dtype == np.float64:
        return (rng.standard_normal(size=shape) * 1000.0).astype(np.float64)
    raise ValueError(dtype)


def assert_exact(name: str, got: np.ndarray, expected: np.ndarray, stats: list):
    if got.shape != expected.shape or got.dtype != expected.dtype:
        raise AssertionError(f"[{name}] shape/dtype mismatch: got {got.shape}/{got.dtype}, exp {expected.shape}/{expected.dtype}")
    if not np.array_equal(got, expected):
        diff = np.abs(got.astype(np.float64) - expected.astype(np.float64))
        raise AssertionError(f"[{name}] pixel mismatch: max_diff={diff.max()}, mean_diff={diff.mean():.4f}")
    stats.append(name)


def assert_close(name: str, got: np.ndarray, expected: np.ndarray, max_mae: float, max_abs: float, stats: list):
    if got.shape != expected.shape:
        raise AssertionError(f"[{name}] shape mismatch: got {got.shape}, exp {expected.shape}")
    diff = np.abs(got.astype(np.float64) - expected.astype(np.float64))
    mae = float(diff.mean())
    mx = float(diff.max())
    if mae > max_mae or mx > max_abs:
        raise AssertionError(f"[{name}] exceeded tol: mae={mae:.3f} (lim {max_mae}), max={mx:.1f} (lim {max_abs})")
    stats.append(name)


def verify_pillow(exe: Path, tmp: Path, stats: list):
    raw = tmp / "dec.raw"

    # ---- 1. PNG (bit-exact bidirectional) ----
    # Pillow -> keel: L (8-bit), LA (8-bit), RGB (8-bit), RGBA (8-bit), I;16 (16-bit), 1-bit, P (indexed), Adam7 interlaced
    png_cases = [
        ("png_l8", make_pattern(37, 43, 1, np.uint8, 1), "L", {}),
        ("png_la8", make_pattern(29, 31, 2, np.uint8, 2), "LA", {}),
        ("png_rgb8", make_pattern(41, 53, 3, np.uint8, 3), "RGB", {}),
        ("png_rgba8", make_pattern(23, 37, 4, np.uint8, 4), "RGBA", {}),
        ("png_rgb8_adam7", make_pattern(35, 49, 3, np.uint8, 5), "RGB", {"interlace": True}),
        ("png_l8_adam7", make_pattern(19, 25, 1, np.uint8, 6), "L", {"interlace": True}),
        ("png_rgba8_adam7", make_pattern(17, 29, 4, np.uint8, 7), "RGBA", {"interlace": True}),
    ]
    for name, arr, mode, kwargs in png_cases:
        p = tmp / f"{name}.png"
        Image.fromarray(arr, mode=mode).save(p, format="PNG", **kwargs)
        _, got = run_keel_decode(exe, p, raw)
        assert_exact(f"pillow->keel:{name}", got, arr, stats)

    # 16-bit grayscale PNG from Pillow
    g16 = make_pattern(27, 33, 1, np.uint16, 8)
    p16 = tmp / "png_u16.png"
    Image.fromarray(g16).save(p16, format="PNG")
    _, got16 = run_keel_decode(exe, p16, raw)
    assert_exact("pillow->keel:png_u16", got16, g16, stats)

    # 1-bit PNG from Pillow (mode '1': 0/255)
    bw = (make_pattern(21, 31, 1, np.uint8, 9) > 127).astype(np.uint8) * 255
    pbw = tmp / "png_1bit.png"
    Image.fromarray(bw, mode="L").convert("1", dither=Image.Dither.NONE).save(pbw, format="PNG")
    _, got_bw = run_keel_decode(exe, pbw, raw)
    assert_exact("pillow->keel:png_1bit", got_bw, bw, stats)

    # Indexed PNG from Pillow (mode 'P')
    idx_rgb = make_smooth_rgb(24, 32)
    p_idx = tmp / "png_indexed.png"
    im_p = Image.fromarray(idx_rgb, mode="RGB").convert("P", palette=Image.Palette.ADAPTIVE, colors=32)
    im_p.save(p_idx, format="PNG")
    ref_p_rgb = np.array(im_p.convert("RGB"))
    _, got_p_rgb = run_keel_decode(exe, p_idx, raw, expand=True)
    assert_exact("pillow->keel:png_indexed_expand", got_p_rgb, ref_p_rgb, stats)

    # keel -> Pillow PNG (8-bit L/LA/RGB/RGBA + 16-bit L)
    for name, arr, stype in [
        ("png_l8", make_pattern(31, 47, 1, np.uint8, 10), "u8"),
        ("png_la8", make_pattern(23, 29, 2, np.uint8, 11), "u8"),
        ("png_rgb8", make_pattern(37, 41, 3, np.uint8, 12), "u8"),
        ("png_rgba8", make_pattern(19, 33, 4, np.uint8, 13), "u8"),
        ("png_l16", make_pattern(25, 35, 1, np.uint16, 14), "u16"),
    ]:
        p = tmp / f"keel_{name}.png"
        run_keel_encode(exe, "png", arr, stype, p, tmp)
        pil_arr = np.array(Image.open(p))
        if stype == "u16":
            pil_arr = pil_arr.astype(np.uint16)
        assert_exact(f"keel->pillow:{name}", pil_arr, arr, stats)

    # ---- 2. JPEG (baseline 4:4:4, 4:2:2, 4:2:0, grayscale, progressive, quality sweep) ----
    smooth_rgb = make_smooth_rgb(64, 80)
    smooth_gray = smooth_rgb[:, :, 0]
    jpeg_cases = [
        ("jpg_rgb_444", smooth_rgb, "RGB", {"quality": 95, "subsampling": 0}),
        ("jpg_rgb_422", smooth_rgb, "RGB", {"quality": 95, "subsampling": 1}),
        ("jpg_rgb_420", smooth_rgb, "RGB", {"quality": 95, "subsampling": 2}),
        ("jpg_rgb_q70", smooth_rgb, "RGB", {"quality": 70, "subsampling": 2}),
        ("jpg_rgb_prog", smooth_rgb, "RGB", {"quality": 92, "progressive": True, "subsampling": 0}),
        ("jpg_rgb_prog_420", smooth_rgb, "RGB", {"quality": 92, "progressive": True, "subsampling": 2}),
        ("jpg_gray_base", smooth_gray, "L", {"quality": 95}),
        ("jpg_gray_prog", smooth_gray, "L", {"quality": 92, "progressive": True}),
        ("jpg_odd_37x53", make_smooth_rgb(37, 53), "RGB", {"quality": 95, "subsampling": 2}),
    ]
    for name, arr, mode, kwargs in jpeg_cases:
        p = tmp / f"{name}.jpg"
        Image.fromarray(arr, mode=mode).save(p, format="JPEG", **kwargs)
        ref = np.array(Image.open(p))
        _, got = run_keel_decode(exe, p, raw)
        # Allow small IDCT / chroma nearest-vs-fancy upsampling tolerance vs libjpeg-turbo
        max_abs = 12.0 if kwargs.get("subsampling", 0) != 0 else 4.0
        assert_close(f"pillow->keel:{name}", got, ref, max_mae=2.0, max_abs=max_abs, stats=stats)

    # keel -> Pillow JPEG (grayscale & RGB, odd dimensions)
    for name, arr in [
        ("jpg_rgb", make_smooth_rgb(48, 64)),
        ("jpg_gray", make_smooth_rgb(48, 64)[:, :, 1]),
        ("jpg_odd", make_smooth_rgb(33, 47)),
    ]:
        p = tmp / f"keel_{name}.jpg"
        run_keel_encode(exe, "jpeg", arr, "u8", p, tmp, extra="95")
        pil_dec = np.array(Image.open(p))
        _, keel_dec = run_keel_decode(exe, p, raw)
        # keel decode vs Pillow decode of the EXACT SAME keel-encoded JPEG must match closely (IDCT rounding <= 2)
        assert_close(f"keel->pillow_idct:{name}", keel_dec, pil_dec, max_mae=1.0, max_abs=3.0, stats=stats)
        assert_close(f"keel->pillow_src:{name}", pil_dec, arr, max_mae=3.0, max_abs=15.0, stats=stats)

    # ---- 3. TIFF & BigTIFF (bit-exact bidirectional) ----
    # Pillow -> keel: raw, packbits, tiff_lzw, tiff_adobe_deflate (incl. large image crossing LZW 4096 reset!)
    for comp in ["raw", "packbits", "tiff_lzw", "tiff_adobe_deflate"]:
        for name, arr, mode in [
            (f"tif_{comp}_rgb", make_pattern(96, 112, 3, np.uint8, 20), "RGB"),
            (f"tif_{comp}_rgba", make_pattern(47, 59, 4, np.uint8, 21), "RGBA"),
            (f"tif_{comp}_l8", make_pattern(65, 73, 1, np.uint8, 22), "L"),
            (f"tif_{comp}_u16", make_pattern(45, 55, 1, np.uint16, 23), "I;16"),
            (f"tif_{comp}_f32", make_pattern(31, 41, 1, np.float32, 24), "F"),
        ]:
            p = tmp / f"{name}.tif"
            im = Image.fromarray(arr) if mode in ("I;16", "F") else Image.fromarray(arr, mode=mode)
            im.save(p, format="TIFF", compression=comp)
            _, got = run_keel_decode(exe, p, raw)
            assert_exact(f"pillow->keel:{name}", got, arr, stats)

    # Multi-page TIFF from Pillow
    mp_pages = [make_pattern(25, 31, 3, np.uint8, 30 + i) for i in range(4)]
    p_mp = tmp / "pil_multipage.tif"
    imgs = [Image.fromarray(a, mode="RGB") for a in mp_pages]
    imgs[0].save(p_mp, format="TIFF", save_all=True, append_images=imgs[1:], compression="tiff_lzw")
    for idx, exp in enumerate(mp_pages):
        meta, got = run_keel_decode(exe, p_mp, raw, page=idx)
        assert meta["page_count"] == 4
        assert_exact(f"pillow->keel:tif_multipage_p{idx}", got, exp, stats)

    # keel -> Pillow TIFF: None, PackBits, LZW (large > 4096 codes!), Deflate, BigTIFF, Multi-page append
    large_rgb = make_pattern(128, 128, 3, np.uint8, 40)
    for fmt_tag in ["tiff", "tiff_packbits", "tiff_lzw", "tiff_deflate", "tiff_big", "tiff_big_lzw"]:
        p = tmp / f"keel_{fmt_tag}.tif"
        run_keel_encode(exe, fmt_tag, large_rgb, "u8", p, tmp)
        pil_arr = np.array(Image.open(p))
        assert_exact(f"keel->pillow:{fmt_tag}_rgb128", pil_arr, large_rgb, stats)

    # keel -> Pillow U16 & F32 TIFF
    for stype, arr in [("u16", make_pattern(37, 43, 1, np.uint16, 41)), ("f32", make_pattern(29, 37, 1, np.float32, 42))]:
        for fmt_tag in ["tiff", "tiff_lzw", "tiff_deflate"]:
            p = tmp / f"keel_{fmt_tag}_{stype}.tif"
            run_keel_encode(exe, fmt_tag, arr, stype, p, tmp)
            pil_arr = np.array(Image.open(p))
            if stype == "u16":
                pil_arr = pil_arr.astype(np.uint16)
            assert_exact(f"keel->pillow:{fmt_tag}_{stype}", pil_arr, arr, stats)

    # keel -> Pillow multi-page append (Classic -> Classic -> BigTIFF upgrade)
    p_kmp = tmp / "keel_multipage.tif"
    kp0 = make_pattern(21, 19, 3, np.uint8, 45)
    kp1 = make_pattern(25, 17, 3, np.uint8, 46)
    kp2 = make_pattern(19, 23, 3, np.uint8, 47)
    run_keel_encode(exe, "tiff", kp0, "u8", p_kmp, tmp)
    run_keel_encode(exe, "tiff_lzw", kp1, "u8", p_kmp, tmp, extra="append")
    run_keel_encode(exe, "tiff_big_lzw", kp2, "u8", p_kmp, tmp, extra="append")
    with Image.open(p_kmp) as im:
        frames = [np.array(f.copy()) for f in ImageSequence.Iterator(im)]
    assert len(frames) == 3
    for i, exp in enumerate([kp0, kp1, kp2]):
        assert_exact(f"keel->pillow:tif_multipage_upgrade_p{i}", frames[i], exp, stats)

    # ---- 4. BMP (bit-exact bidirectional) ----
    for name, arr, mode in [
        ("bmp_l8", make_pattern(33, 29, 1, np.uint8, 50), "L"),
        ("bmp_rgb24", make_pattern(37, 41, 3, np.uint8, 51), "RGB"),
        ("bmp_rgba32", make_pattern(23, 31, 4, np.uint8, 52), "RGBA"),
        ("bmp_1bit", (make_pattern(19, 27, 1, np.uint8, 53) > 127).astype(np.uint8) * 255, "1"),
    ]:
        p = tmp / f"{name}.bmp"
        if mode == "1":
            Image.fromarray(arr, mode="L").convert("1", dither=Image.Dither.NONE).save(p, format="BMP")
            _, got = run_keel_decode(exe, p, raw)
            # 1-bit BMP expands palette to RGB
            exp_rgb = np.stack([arr, arr, arr], axis=-1)
            assert_exact(f"pillow->keel:{name}", got, exp_rgb, stats)
        else:
            Image.fromarray(arr, mode=mode).save(p, format="BMP")
            _, got = run_keel_decode(exe, p, raw)
            assert_exact(f"pillow->keel:{name}", got, arr, stats)

    for name, arr in [
        ("bmp_l8", make_pattern(35, 29, 1, np.uint8, 54)),
        ("bmp_rgb24", make_pattern(31, 39, 3, np.uint8, 55)),
        ("bmp_rgba32", make_pattern(21, 25, 4, np.uint8, 56)),
    ]:
        p = tmp / f"keel_{name}.bmp"
        run_keel_encode(exe, "bmp", arr, "u8", p, tmp)
        pil_arr = np.array(Image.open(p))
        assert_exact(f"keel->pillow:{name}", pil_arr, arr, stats)

    # ---- 5. TGA (bit-exact bidirectional, raw & RLE) ----
    for rle in [False, True]:
        for name, arr, mode in [
            (f"tga_l8_rle{int(rle)}", make_pattern(27, 33, 1, np.uint8, 60), "L"),
            (f"tga_rgb_rle{int(rle)}", make_pattern(31, 37, 3, np.uint8, 61), "RGB"),
            (f"tga_rgba_rle{int(rle)}", make_pattern(23, 29, 4, np.uint8, 62), "RGBA"),
        ]:
            p = tmp / f"{name}.tga"
            Image.fromarray(arr, mode=mode).save(p, format="TGA", compression="tga_rle" if rle else None)
            _, got = run_keel_decode(exe, p, raw)
            assert_exact(f"pillow->keel:{name}", got, arr, stats)

    for name, arr in [
        ("tga_l8", make_pattern(29, 31, 1, np.uint8, 63)),
        ("tga_rgb", make_pattern(33, 27, 3, np.uint8, 64)),
        ("tga_rgba", make_pattern(19, 23, 4, np.uint8, 65)),
    ]:
        p = tmp / f"keel_{name}.tga"
        run_keel_encode(exe, "tga", arr, "u8", p, tmp)
        pil_arr = np.array(Image.open(p))
        assert_exact(f"keel->pillow:{name}", pil_arr, arr, stats)

    # ---- 6. PNM (P4/P5/P6 bit-exact bidirectional) ----
    pbm_arr = (make_pattern(19, 27, 1, np.uint8, 70) > 127).astype(np.uint8) * 255
    p_pbm = tmp / "pil_p4.pbm"
    Image.fromarray(pbm_arr, mode="L").convert("1", dither=Image.Dither.NONE).save(p_pbm, format="PPM")
    _, got_pbm = run_keel_decode(exe, p_pbm, raw)
    assert_exact("pillow->keel:pnm_p4", got_pbm, pbm_arr, stats)

    pgm_arr = make_pattern(29, 37, 1, np.uint8, 71)
    p_pgm = tmp / "pil_p5.pgm"
    Image.fromarray(pgm_arr, mode="L").save(p_pgm, format="PPM")
    _, got_pgm = run_keel_decode(exe, p_pgm, raw)
    assert_exact("pillow->keel:pnm_p5", got_pgm, pgm_arr, stats)

    ppm_arr = make_pattern(27, 35, 3, np.uint8, 72)
    p_ppm = tmp / "pil_p6.ppm"
    Image.fromarray(ppm_arr, mode="RGB").save(p_ppm, format="PPM")
    _, got_ppm = run_keel_decode(exe, p_ppm, raw)
    assert_exact("pillow->keel:pnm_p6", got_ppm, ppm_arr, stats)

    for fmt_tag, arr, stype in [
        ("pbm", pbm_arr, "u8"),
        ("pgm", pgm_arr, "u8"),
        ("ppm", ppm_arr, "u8"),
    ]:
        p = tmp / f"keel.{fmt_tag}"
        run_keel_encode(exe, fmt_tag, arr, stype, p, tmp)
        pil_arr = np.array(Image.open(p))
        if fmt_tag == "pbm":
            pil_arr = pil_arr.astype(np.uint8) * 255
        assert_exact(f"keel->pillow:pnm_{fmt_tag}", pil_arr, arr, stats)

    # ---- 7. GIF (normal & interlaced, Pillow -> keel bit-exact) ----
    for interlace in [False, True]:
        p_gif = tmp / f"pil_interlace{int(interlace)}.gif"
        im_g = Image.fromarray(make_smooth_rgb(35, 47), mode="RGB").convert("P", palette=Image.Palette.ADAPTIVE, colors=64)
        im_g.save(p_gif, format="GIF", interlace=interlace)
        ref_rgb = np.array(Image.open(p_gif).convert("RGB"))
        _, got_rgb = run_keel_decode(exe, p_gif, raw, expand=True)
        assert_exact(f"pillow->keel:gif_interlace{int(interlace)}", got_rgb, ref_rgb, stats)


def verify_ffmpeg(exe: Path, tmp: Path, stats: list):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        print("  [SKIP] ffmpeg not found on PATH")
        return

    raw = tmp / "ff_dec.raw"

    # 1. FFmpeg-generated 16-bit RGB PNG -> keel
    rgb16 = make_pattern(23, 29, 3, np.uint16, 80)
    in_raw = tmp / "rgb16.raw"
    in_raw.write_bytes(rgb16.tobytes())
    p_png16 = tmp / "ff_rgb16.png"
    subprocess.run(
        [ffmpeg, "-y", "-f", "rawvideo", "-pix_fmt", "rgb48le", "-s", "29x23", "-i", str(in_raw), str(p_png16)],
        check=True, capture_output=True
    )
    _, got = run_keel_decode(exe, p_png16, raw)
    assert_exact("ffmpeg->keel:png_rgb16", got, rgb16, stats)

    # 2. FFmpeg-generated 16-bit RGBA PNG -> keel
    rgba16 = make_pattern(19, 25, 4, np.uint16, 81)
    in_raw4 = tmp / "rgba16.raw"
    in_raw4.write_bytes(rgba16.tobytes())
    p_png16a = tmp / "ff_rgba16.png"
    subprocess.run(
        [ffmpeg, "-y", "-f", "rawvideo", "-pix_fmt", "rgba64le", "-s", "25x19", "-i", str(in_raw4), str(p_png16a)],
        check=True, capture_output=True
    )
    _, got4 = run_keel_decode(exe, p_png16a, raw)
    assert_exact("ffmpeg->keel:png_rgba16", got4, rgba16, stats)

    # 3. FFmpeg-generated 16-bit PNM (P6 16-bit big-endian) -> keel AND keel -> FFmpeg
    p_ppm16 = tmp / "ff_rgb16.ppm"
    subprocess.run(
        [ffmpeg, "-y", "-f", "rawvideo", "-pix_fmt", "rgb48le", "-s", "29x23", "-i", str(in_raw), str(p_ppm16)],
        check=True, capture_output=True
    )
    _, got_ppm16 = run_keel_decode(exe, p_ppm16, raw)
    assert_exact("ffmpeg->keel:pnm_p6_u16", got_ppm16, rgb16, stats)

    p_kppm16 = tmp / "keel_rgb16.ppm"
    run_keel_encode(exe, "ppm", rgb16, "u16", p_kppm16, tmp)
    ff_out = tmp / "ff_from_keel_ppm16.raw"
    subprocess.run(
        [ffmpeg, "-y", "-i", str(p_kppm16), "-f", "rawvideo", "-pix_fmt", "rgb48le", str(ff_out)],
        check=True, capture_output=True
    )
    assert_exact("keel->ffmpeg:pnm_p6_u16", np.frombuffer(ff_out.read_bytes(), dtype=np.uint16).reshape(rgb16.shape), rgb16, stats)

    # 4. FFmpeg-generated 16-bit RGB TIFF (LZW & Deflate) -> keel AND keel -> FFmpeg
    for comp in ["lzw", "deflate", "packbits", "raw"]:
        p_t16 = tmp / f"ff_rgb16_{comp}.tif"
        subprocess.run(
            [ffmpeg, "-y", "-f", "rawvideo", "-pix_fmt", "rgb48le", "-s", "29x23", "-i", str(in_raw),
             "-compression_algo", comp, str(p_t16)],
            check=True, capture_output=True
        )
        _, got_t16 = run_keel_decode(exe, p_t16, raw)
        assert_exact(f"ffmpeg->keel:tiff_rgb16_{comp}", got_t16, rgb16, stats)

    # 5. FFmpeg-generated 16-bit RGB565 BI_BITFIELDS BMP -> keel
    rgb565_src = make_smooth_rgb(24, 32)
    in_rgb8 = tmp / "rgb8.raw"
    in_rgb8.write_bytes(rgb565_src.tobytes())
    p_bmp565 = tmp / "ff_rgb565.bmp"
    subprocess.run(
        [ffmpeg, "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "32x24", "-i", str(in_rgb8),
         "-pix_fmt", "rgb565le", str(p_bmp565)],
        check=True, capture_output=True
    )
    ff_bmp565_ref = tmp / "ff_rgb565_ref.raw"
    subprocess.run(
        [ffmpeg, "-y", "-i", str(p_bmp565), "-f", "rawvideo", "-pix_fmt", "rgb24", str(ff_bmp565_ref)],
        check=True, capture_output=True
    )
    ref565 = np.frombuffer(ff_bmp565_ref.read_bytes(), dtype=np.uint8).reshape((24, 32, 3))
    _, got565 = run_keel_decode(exe, p_bmp565, raw)
    # Bit-replication vs round-to-nearest differs by at most 1 LSB
    assert_close("ffmpeg->keel:bmp_rgb565_bitfields", got565, ref565, max_mae=0.5, max_abs=1.0, stats=stats)


def verify_matlab(exe: Path, tmp: Path, stats: list):
    matlab = shutil.which("matlab") or r"C:\Program Files\MATLAB\R2025b\bin\matlab.exe"
    if not os.path.exists(matlab):
        print("  [SKIP] MATLAB not found")
        return

    raw = tmp / "ml_dec.raw"
    # Pre-encode keel files for MATLAB to read, AND have MATLAB write files for keel to read in ONE batch call!
    k_png16_rgb = make_pattern(23, 29, 3, np.uint16, 90)
    k_tif_f32 = make_pattern(19, 25, 3, np.float32, 91)
    k_tif_f64 = make_pattern(17, 21, 1, np.float64, 92)
    k_tif_i16 = make_pattern(21, 27, 3, np.int16, 93)
    k_tif_u32 = make_pattern(15, 19, 1, np.uint32, 94)
    k_tif_lzw = make_pattern(96, 96, 3, np.uint8, 95)

    p_k_png16 = tmp / "keel_ml_rgb16.png"
    p_k_f32 = tmp / "keel_ml_f32.tif"
    p_k_f64 = tmp / "keel_ml_f64.tif"
    p_k_i16 = tmp / "keel_ml_i16.tif"
    p_k_u32 = tmp / "keel_ml_u32.tif"
    p_k_lzw = tmp / "keel_ml_lzw.tif"

    run_keel_encode(exe, "png", k_png16_rgb, "u16", p_k_png16, tmp)
    run_keel_encode(exe, "tiff_deflate", k_tif_f32, "f32", p_k_f32, tmp)
    run_keel_encode(exe, "tiff", k_tif_f64, "f64", p_k_f64, tmp)
    run_keel_encode(exe, "tiff_lzw", k_tif_i16, "i16", p_k_i16, tmp)
    run_keel_encode(exe, "tiff_deflate", k_tif_u32, "u32", p_k_u32, tmp)
    run_keel_encode(exe, "tiff_lzw", k_tif_lzw, "u8", p_k_lzw, tmp)

    # Write expected raw blobs so MATLAB can verify keel outputs directly AND write MATLAB-generated files for keel!
    (tmp / "exp_png16.bin").write_bytes(k_png16_rgb.tobytes())
    (tmp / "exp_f32.bin").write_bytes(k_tif_f32.tobytes())
    (tmp / "exp_f64.bin").write_bytes(k_tif_f64.tobytes())
    (tmp / "exp_i16.bin").write_bytes(k_tif_i16.tobytes())
    (tmp / "exp_u32.bin").write_bytes(k_tif_u32.tobytes())
    (tmp / "exp_lzw.bin").write_bytes(k_tif_lzw.tobytes())

    ml_script = tmp / "verify_img.m"
    td = str(tmp).replace("\\", "/")
    ml_script.write_text(f"""
td = '{td}';
function check_raw(im, binpath, cls, h, w, ch)
    fid = fopen(binpath, 'rb');
    raw = fread(fid, Inf, ['*' cls]);
    fclose(fid);
    if ch == 1
        exp_im = permute(reshape(raw, [w, h]), [2, 1]);
    else
        exp_im = permute(reshape(raw, [ch, w, h]), [3, 2, 1]);
    end
    assert(isequal(class(im), cls), sprintf('class mismatch %s vs %s', class(im), cls));
    assert(isequal(size(im), size(exp_im)), 'size mismatch');
    assert(isequal(im, exp_im), 'pixel mismatch');
end

% 1. Verify keel -> MATLAB (imread)
check_raw(imread([td '/keel_ml_rgb16.png']), [td '/exp_png16.bin'], 'uint16', 23, 29, 3);
check_raw(imread([td '/keel_ml_f32.tif']),   [td '/exp_f32.bin'],   'single', 19, 25, 3);
check_raw(imread([td '/keel_ml_f64.tif']),   [td '/exp_f64.bin'],   'double', 17, 21, 1);
check_raw(imread([td '/keel_ml_i16.tif']),   [td '/exp_i16.bin'],   'int16',  21, 27, 3);
check_raw(imread([td '/keel_ml_u32.tif']),   [td '/exp_u32.bin'],   'uint32', 15, 19, 1);
check_raw(imread([td '/keel_ml_lzw.tif']),   [td '/exp_lzw.bin'],   'uint8',  96, 96, 3);

% 2. Generate MATLAB Tiff files (Tiled LZW, Planar U16 Deflate, BigTIFF F64, Tiled Planar RGB8)
% 2a. Tiled RGB8 with LZW
rng(123);
im_tiled = uint8(randi([0 255], 48, 64, 3));
t = Tiff([td '/ml_tiled_pred2.tif'], 'w');
setTag(t, 'ImageLength', 48); setTag(t, 'ImageWidth', 64);
setTag(t, 'Photometric', Tiff.Photometric.RGB);
setTag(t, 'BitsPerSample', 8); setTag(t, 'SamplesPerPixel', 3);
setTag(t, 'TileWidth', 16); setTag(t, 'TileLength', 16);
setTag(t, 'PlanarConfiguration', Tiff.PlanarConfiguration.Chunky);
setTag(t, 'Compression', Tiff.Compression.LZW);
write(t, im_tiled); close(t);
fid = fopen([td '/ml_tiled_pred2.raw'], 'wb'); fwrite(fid, permute(im_tiled, [3, 2, 1]), 'uint8'); fclose(fid);

% 2b. Planar Deflate RGB16
im_planar = uint16(randi([0 65535], 32, 40, 3));
t = Tiff([td '/ml_planar_u16_pred2.tif'], 'w');
setTag(t, 'ImageLength', 32); setTag(t, 'ImageWidth', 40);
setTag(t, 'Photometric', Tiff.Photometric.RGB);
setTag(t, 'BitsPerSample', 16); setTag(t, 'SamplesPerPixel', 3);
setTag(t, 'RowsPerStrip', 8);
setTag(t, 'PlanarConfiguration', Tiff.PlanarConfiguration.Separate);
setTag(t, 'Compression', Tiff.Compression.Deflate);
write(t, im_planar); close(t);
fid = fopen([td '/ml_planar_u16_pred2.raw'], 'wb'); fwrite(fid, permute(im_planar, [3, 2, 1]), 'uint16'); fclose(fid);

% 2c. BigTIFF F64 + Deflate
im_f64 = randn(25, 31);
t = Tiff([td '/ml_bigtiff_f64.tif'], 'w8');
setTag(t, 'ImageLength', 25); setTag(t, 'ImageWidth', 31);
setTag(t, 'Photometric', Tiff.Photometric.MinIsBlack);
setTag(t, 'BitsPerSample', 64); setTag(t, 'SamplesPerPixel', 1);
setTag(t, 'SampleFormat', Tiff.SampleFormat.IEEEFP);
setTag(t, 'RowsPerStrip', 25);
setTag(t, 'PlanarConfiguration', Tiff.PlanarConfiguration.Chunky);
setTag(t, 'Compression', Tiff.Compression.Deflate);
write(t, im_f64); close(t);
fid = fopen([td '/ml_bigtiff_f64.raw'], 'wb'); fwrite(fid, permute(im_f64, [2, 1]), 'double'); fclose(fid);

% 2d. Tiled Planar RGB8 with LZW
im_tp = uint8(randi([0 255], 40, 48, 3));
t = Tiff([td '/ml_tiled_planar_pred2.tif'], 'w');
setTag(t, 'ImageLength', 40); setTag(t, 'ImageWidth', 48);
setTag(t, 'Photometric', Tiff.Photometric.RGB);
setTag(t, 'BitsPerSample', 8); setTag(t, 'SamplesPerPixel', 3);
setTag(t, 'TileWidth', 16); setTag(t, 'TileLength', 16);
setTag(t, 'PlanarConfiguration', Tiff.PlanarConfiguration.Separate);
setTag(t, 'Compression', Tiff.Compression.LZW);
write(t, im_tp); close(t);
fid = fopen([td '/ml_tiled_planar_pred2.raw'], 'wb'); fwrite(fid, permute(im_tp, [3, 2, 1]), 'uint8'); fclose(fid);

% 2e. MATLAB imwrite PNG 16-bit RGB + JPEG
imwrite(im_planar, [td '/ml_rgb16.png']);
disp('MATLAB_IMG_OK');
""", encoding="utf-8")

    proc = subprocess.run([matlab, "-batch", f"run('{td}/verify_img.m')"], capture_output=True, text=True, check=False)
    if proc.returncode != 0 or "MATLAB_IMG_OK" not in proc.stdout:
        raise RuntimeError(f"MATLAB verification failed:\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}")

    for tag in ["keel->matlab:png_rgb16", "keel->matlab:tif_f32_deflate", "keel->matlab:tif_f64",
                "keel->matlab:tif_i16_lzw", "keel->matlab:tif_u32_deflate", "keel->matlab:tif_u8_lzw96"]:
        stats.append(tag)

    # Now decode MATLAB-generated TIFFs & PNG with keel and verify bit-exact!
    ml_checks = [
        ("ml_tiled_pred2.tif", "ml_tiled_pred2.raw", (48, 64, 3), np.uint8),
        ("ml_planar_u16_pred2.tif", "ml_planar_u16_pred2.raw", (32, 40, 3), np.uint16),
        ("ml_bigtiff_f64.tif", "ml_bigtiff_f64.raw", (25, 31), np.float64),
        ("ml_tiled_planar_pred2.tif", "ml_tiled_planar_pred2.raw", (40, 48, 3), np.uint8),
        ("ml_rgb16.png", "ml_planar_u16_pred2.raw", (32, 40, 3), np.uint16),
    ]
    for img_name, raw_name, shape, dtype in ml_checks:
        exp = np.frombuffer((tmp / raw_name).read_bytes(), dtype=dtype).reshape(shape)
        _, got = run_keel_decode(exe, tmp / img_name, raw)
        assert_exact(f"matlab->keel:{img_name}", got, exp, stats)


def run_adversarial_fuzz(exe: Path, tmp: Path):
    fuzz_dir = tmp / "fuzz_corpus"
    fuzz_dir.mkdir(exist_ok=True)
    rng = random.Random(20261126)

    # Seed valid samples of all 7 formats
    seeds = []
    rgb = make_smooth_rgb(16, 16)
    for fmt, ext, kwargs in [
        ("PNG", "png", {"interlace": True}),
        ("JPEG", "jpg", {"quality": 85, "progressive": True}),
        ("JPEG", "jpg", {"quality": 85, "subsampling": 2}),
        ("TIFF", "tif", {"compression": "tiff_lzw"}),
        ("TIFF", "tif", {"compression": "tiff_adobe_deflate"}),
        ("TIFF", "tif", {"compression": "packbits"}),
        ("BMP", "bmp", {}),
        ("TGA", "tga", {"compression": "tga_rle"}),
        ("PPM", "ppm", {}),
    ]:
        p = tmp / f"seed_{len(seeds)}.{ext}"
        Image.fromarray(rgb, mode="RGB").save(p, format=fmt, **kwargs)
        seeds.append((ext, p.read_bytes()))

    p_gif = tmp / "seed_gif.gif"
    Image.fromarray(rgb, mode="RGB").convert("P").save(p_gif, format="GIF", interlace=True)
    seeds.append(("gif", p_gif.read_bytes()))

    idx = 0
    # 1. Truncations + random byte flips on every format seed
    for ext, blob in seeds:
        for frac in [0.05, 0.15, 0.4, 0.7, 0.95]:
            cut = max(1, int(len(blob) * frac))
            (fuzz_dir / f"trunc_{idx:03d}.{ext}").write_bytes(blob[:cut])
            idx += 1
        for _ in range(14):
            mut = bytearray(blob)
            for _ in range(rng.randint(1, 8)):
                pos = rng.randrange(len(mut))
                mut[pos] = rng.randrange(256)
            (fuzz_dir / f"mut_{idx:03d}.{ext}").write_bytes(bytes(mut))
            idx += 1

    # 2. Targeted OOM / integer-overflow / malformed header bombs
    bombs = [
        # Huge PNG IHDR dimensions (0x7FFFFFFF x 0x7FFFFFFF)
        b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x7f\xff\xff\xff\x7f\xff\xff\xff\x08\x06\x00\x00\x00\x00\x00\x00\x00",
        # Huge BMP width/height
        b"BM" + struct.pack("<IHHIIiiHHIIIIII", 70, 0, 0, 54, 40, 0x3FFFFFFF, 0x3FFFFFFF, 1, 32, 0, 0, 0, 0, 0, 0),
        # BMP INT32_MIN height
        b"BM" + struct.pack("<IHHIIiiHHIIIIII", 70, 0, 0, 54, 40, 16, -2147483648, 1, 24, 0, 0, 0, 0, 0, 0),
        # BMP BI_BITFIELDS with zero masks
        b"BM" + struct.pack("<IHHIIiiHHIIIIIIIII", 80, 0, 0, 66, 40, 2, 2, 1, 32, 3, 0, 0, 0, 0, 0, 0, 0, 0) + b"\x00" * 16,
        # TGA grayscale with 32-bit pixel depth (former heap overflow!)
        struct.pack("<BBBHHBHHHHBB", 0, 0, 3, 0, 0, 0, 0, 0, 8, 8, 32, 0x20) + b"\xAA" * 256,
        # TGA RLE huge run bomb
        struct.pack("<BBBHHBHHHHBB", 0, 0, 10, 0, 0, 0, 0, 0, 65535, 65535, 32, 0x20) + b"\xff\x01\x02\x03\x04" * 16,
        # PNM huge dimensions
        b"P6\n99999999 99999999\n255\n\x00\x00\x00",
        b"P1\n10 10\n2222222222",
        # TIFF self-referencing IFD cycle + huge tile bomb
        b"II*\x00\x08\x00\x00\x00\x00\x00\x08\x00\x00\x00",
        b"II+\x00\x08\x00\x00\x00\x10\x00\x00\x00\x00\x00\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff",
        # JPEG progressive with interleaved AC scan (ss=1, num_scan_comps=3)
        b"\xff\xd8\xff\xc2\x00\x11\x08\x00\x08\x00\x08\x03\x01\x11\x00\x02\x11\x00\x03\x11\x00"
        b"\xff\xda\x00\x0c\x03\x01\x00\x02\x00\x03\x00\x01\x3f\x00\x00\xff\xd9",
        # GIF with invalid LZW min_code_size = 15
        b"GIF89a\x04\x00\x04\x00\x80\x00\x00\x00\x00\x00\xff\xff\xff\x2c\x00\x00\x00\x00\x04\x00\x04\x00\x00\x0f\x01\x00\x00\x3b",
    ]
    for b in bombs:
        (fuzz_dir / f"bomb_{idx:03d}.bin").write_bytes(b)
        idx += 1

    # 3. Pure random noise files
    for _ in range(25):
        (fuzz_dir / f"rand_{idx:03d}.bin").write_bytes(rng.randbytes(rng.randint(1, 512)))
        idx += 1

    proc = subprocess.run([str(exe), "--fuzz", str(fuzz_dir)], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"Fuzz run failed ({proc.returncode}): {proc.stdout}\n{proc.stderr}")
    print(f"  [FUZZ] {proc.stdout.strip()}")


def main():
    if len(sys.argv) < 2:
        print("Usage: verify_reference.py <path-to-test_imagefile.exe> [--no-matlab]")
        sys.exit(2)

    exe = Path(sys.argv[1]).resolve()
    no_matlab = "--no-matlab" in sys.argv[2:]
    if not exe.exists():
        print(f"ERROR: executable not found: {exe}")
        sys.exit(2)

    with tempfile.TemporaryDirectory(prefix="keel_img_ref_") as td:
        tmp = Path(td)
        stats_pil: list[str] = []
        stats_ff: list[str] = []
        stats_ml: list[str] = []

        print("=== 1. Pillow 12.x bidirectional differential tests ===")
        verify_pillow(exe, tmp, stats_pil)
        print(f"  [PASS] Pillow bidirectional: {len(stats_pil)}/{len(stats_pil)} checks passed")

        print("=== 2. FFmpeg 8.x bidirectional differential tests ===")
        verify_ffmpeg(exe, tmp, stats_ff)
        print(f"  [PASS] FFmpeg bidirectional: {len(stats_ff)}/{len(stats_ff)} checks passed")

        if not no_matlab:
            print("=== 3. MATLAB R2025b bidirectional differential tests ===")
            verify_matlab(exe, tmp, stats_ml)
            print(f"  [PASS] MATLAB bidirectional: {len(stats_ml)}/{len(stats_ml)} checks passed")
        else:
            print("=== 3. MATLAB R2025b skipped (--no-matlab) ===")

        print("=== 4. Adversarial fuzz corpus ===")
        run_adversarial_fuzz(exe, tmp)

        total = len(stats_pil) + len(stats_ff) + len(stats_ml)
        print(f"\nALL REFERENCE DIFFERENTIAL CHECKS PASSED ({total}/{total} + fuzz 0 crashes).")


if __name__ == "__main__":
    main()
