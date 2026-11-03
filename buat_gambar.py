# -*- coding: utf-8 -*-
"""
Membuat gambar capture output perbandingan.exe ke folder images/.

  python buat_gambar.py

Isi folder images/:
  01_ringkasan_perbandingan.png   ringkasan hasil 96 soal (dari hasil_perbandingan.txt)
  02_runtime_ringkasan.png        ringkasan runtime tiap metode
  03_runtime_grafik.png           grafik median & rata-rata runtime tiap metode
  04_runtime_per_soal.png         runtime tiap metode untuk tiap soal
  step_by_step/<SOAL>_<n>_<metode>.png   capture --trace langkah demi langkah

Jalankan dulu  .\\perbandingan.exe dataset96 hasil_perbandingan.csv > hasil_perbandingan.txt
supaya ringkasan & runtime di gambar sama dengan file txt.
"""
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "perbandingan.exe" if os.name == "nt" else "perbandingan")
OUT = os.path.join(HERE, "images")
STEP_OUT = os.path.join(OUT, "step_by_step")
TRACE_SOAL = ["CIDI01", "N05"]
TRACE_MODE = [("SOAL", "soal"), ("OPT", "optimal"), ("VAM", "VAM"), ("RBSM", "RBSM"),
              ("SAM", "SAM"), ("CSM", "CSM"), ("VP", "VAM-Pasangan")]
PROMPT = r"PS C:\Users\ROG\Downloads\dataset96-20261103T153724Z-1-001>"

FONT_DIR = r"C:\Windows\Fonts"
FONT = ImageFont.truetype(os.path.join(FONT_DIR, "consola.ttf"), 16)
FONT_B = ImageFont.truetype(os.path.join(FONT_DIR, "consolab.ttf"), 16)
UI = ImageFont.truetype(os.path.join(FONT_DIR, "segoeui.ttf"), 14)
UI_B = ImageFont.truetype(os.path.join(FONT_DIR, "segoeuib.ttf"), 18)

# warna terminal (mirip PowerShell / Windows Terminal)
BG, FG, MUTED, PROMPT_C, CMD_C = "#0c0c0c", "#cccccc", "#767676", "#cccccc", "#f9f1a5"
TITLE_BG, TITLE_FG = "#2b2b2b", "#e0e0e0"


def render_terminal(lines, command, path, note=None):
    """lines: list teks output; command: perintah yang diketik setelah prompt."""
    lh, pad, title_h = 20, 16, 34
    cw = FONT.getbbox("M")[2]
    body = [(PROMPT + " ", command)] + ([("", note)] if note else []) + lines + [(PROMPT, None)]
    maxchars = max(len(b[0]) + len(b[1] or "") if isinstance(b, tuple) else len(b) for b in body)
    w = max(760, pad * 2 + cw * (maxchars + 2))
    h = title_h + pad * 2 + lh * len(body)
    img = Image.new("RGB", (w, h), BG)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w, title_h], fill=TITLE_BG)
    d.text((14, 8), "Windows PowerShell", font=UI, fill=TITLE_FG)
    for k, sym in enumerate(["\u2715", "\u25a1", "\u2013"]):
        d.text((w - 30 - k * 44, 8), sym, font=UI, fill=TITLE_FG)
    y = title_h + pad
    for idx, item in enumerate(body):
        if isinstance(item, tuple):
            pre, cmd = item
            if cmd is None:                       # prompt kosong di akhir
                d.text((pad, y), pre + " ", font=FONT, fill=PROMPT_C)
                x = pad + FONT.getlength(pre + " ")
                d.rectangle([x, y + 2, x + cw - 1, y + lh - 2], fill=FG)   # kursor
            elif pre == "":                       # catatan (bukan output program)
                d.text((pad, y), cmd, font=FONT, fill=MUTED)
            else:
                d.text((pad, y), pre, font=FONT, fill=PROMPT_C)
                d.text((pad + FONT.getlength(pre), y), cmd, font=FONT, fill=CMD_C)
        else:
            d.text((pad, y), item, font=FONT, fill=FG)
        y += lh
    img.save(path)
    print("  ", os.path.relpath(path, HERE))


def section(lines, start, end_incl):
    """ambil baris dari yang diawali `start` sampai yang diawali `end_incl`."""
    i = next(k for k, l in enumerate(lines) if l.startswith(start))
    j = next(k for k in range(i, len(lines)) if lines[k].startswith(end_incl))
    return lines[i:j + 1]


def runtime_chart(summary, path):
    """summary: list (metode, rata2, median). Dua panel bar horizontal, satu skala per panel."""
    W, H = 1100, 440
    surf, ink, ink2, grid = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
    blue, ref = "#2a78d6", "#a3a29c"
    img = Image.new("RGB", (W, H), surf)
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(os.path.join(FONT_DIR, "segoeui.ttf"), 15)
    fs = ImageFont.truetype(os.path.join(FONT_DIR, "segoeui.ttf"), 13)
    d.text((32, 20), "Runtime tiap metode pada 96 soal dataset96", font=UI_B, fill=ink)
    d.text((32, 48), "Mikrodetik (µs) per 1x jalan (tiap metode diulang sampai >= 5 ms lalu dirata-rata). "
                     "Abu-abu = solver optimal sebagai pembanding.", font=fs, fill=ink2)
    panels = [("Median per soal (µs)", 2), ("Rata-rata per soal (µs)", 1)]
    label_w, top, bar_h, gap = 120, 110, 30, 20
    pw = (W - 64 - 40) // 2
    for p, (title, col) in enumerate(panels):
        x0 = 32 + p * (pw + 40)
        d.text((x0, top - 34), title, font=f, fill=ink)
        vmax = max(r[col] for r in summary)
        plot_x0, plot_x1 = x0 + label_w, x0 + pw - 70
        # garis dasar
        d.line([plot_x0, top - 4, plot_x0, top + len(summary) * (bar_h + gap) - gap + 4], fill=grid, width=1)
        for k, row in enumerate(summary):
            name, val = row[0], row[col]
            y = top + k * (bar_h + gap)
            d.text((x0, y + 6), name, font=f, fill=ink)
            L = (plot_x1 - plot_x0) * val / vmax
            color = ref if name.startswith("Optimal") else blue
            if L > 4:
                d.rounded_rectangle([plot_x0, y, plot_x0 + L, y + bar_h], radius=4, fill=color)
                d.rectangle([plot_x0, y, plot_x0 + 4, y + bar_h], fill=color)   # ujung kiri rata ke baseline
            else:
                d.rectangle([plot_x0, y, plot_x0 + max(L, 2), y + bar_h], fill=color)
            d.text((plot_x0 + L + 8, y + 6), f"{val:.2f}", font=f, fill=ink2)
    d.text((32, H - 36), "Rata-rata didominasi soal besar (R1-R5 2x94, MRM_HDTP 12x12, FV33 10x15); "
                         "median lebih mewakili soal kecil yang jumlahnya mayoritas.", font=fs, fill=ink2)
    img.save(path)
    print("  ", os.path.relpath(path, HERE))


def main():
    os.makedirs(STEP_OUT, exist_ok=True)
    txt = os.path.join(HERE, "hasil_perbandingan.txt")
    with open(txt, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()
    cmd_full = r".\perbandingan.exe dataset96 hasil_perbandingan.csv"

    print("Membuat gambar ringkasan & runtime:")
    ring = section(lines, "RINGKASAN (", "  vs CSM")
    render_terminal(ring, cmd_full, os.path.join(OUT, "01_ringkasan_perbandingan.png"),
                    note="... (tabel hasil per soal tidak ditampilkan di gambar ini) ...")
    rt = section(lines, "RINGKASAN RUNTIME", "   Optimal(MCF) = ")
    render_terminal(rt, cmd_full, os.path.join(OUT, "02_runtime_ringkasan.png"),
                    note="... (hasil per soal & runtime per soal tidak ditampilkan di gambar ini) ...")
    rps = section(lines, "RUNTIME PER SOAL", "TIED9")
    render_terminal(rps, cmd_full, os.path.join(OUT, "04_runtime_per_soal.png"),
                    note="... (tabel hasil & ringkasan tidak ditampilkan di gambar ini) ...")

    summary = []
    for l in rt[3:9]:
        parts = l.split()
        summary.append((parts[0], float(parts[2]), float(parts[3])))
    runtime_chart(summary, os.path.join(OUT, "03_runtime_grafik.png"))

    print("Membuat capture step by step:")
    for soal in TRACE_SOAL:
        for n, (mode, label) in enumerate(TRACE_MODE):
            arg = f"dataset96/{soal}.txt"
            res = subprocess.run([EXE, "--trace", arg, mode], cwd=HERE, capture_output=True, text=True)
            if res.returncode != 0:
                sys.exit(f"gagal: {res.stdout}{res.stderr}")
            out = res.stdout.rstrip("\n").splitlines()
            render_terminal(out, rf".\perbandingan.exe --trace dataset96\{soal}.txt {mode}",
                            os.path.join(STEP_OUT, f"{soal}_{n}_{label}.png"))


if __name__ == "__main__":
    main()
