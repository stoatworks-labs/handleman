#!/usr/bin/env python3
"""The stocks' spectral weights.

Writes source/Spectral.h: for each stock, the exposure produced by each of
Smits' seven basis reflectance spectra (white, cyan, magenta, yellow, red,
green, blue) in daylight, normalised so that white exposes to exactly 1. The
shader builds the spectrum of a pixel's linear RGB the way Smits does -- as a
sum of those bases -- so its exposure is the same sum of these seven numbers.
The method and the Smits table are wetplate's (tools/spectral_weights.py
there), unchanged.

    python3 tools/spectral_weights.py            # rewrite source/Spectral.h
    python3 tools/spectral_weights.py --check    # exit 1 if the header differs

--------------------------------------------------------------------- sources

**Orthochromatic.** Silent-era negative stock was orthochromatic: silver
bromide's own sensitivity in the violet and blue, extended into the green by
a sensitising dye (erythrosine), and blind to red. The table below is an
ASSUMED SHAPE that follows that textbook description -- the bromide's band
falling from 410 nm, the gap near 500 nm, the dye's band peaking near 550
nm and gone by 600 nm -- not a digitised published curve. Its numbers are
chosen, and AGENTS.md says so.

**Panchromatic** is written as Rec. 709 luminance (0.2126, 0.7152, 0.0722),
what a modern black-and-white conversion does. Those weights are exact in
Smits' form, since his sum is linear wherever its pieces meet: white 1, red
0.2126, green 0.7152, blue 0.0722, cyan = green + blue, magenta = red +
blue, yellow = red + green.

**The RGB-to-spectrum basis.** Brian Smits, "An RGB-to-Spectrum Conversion
for Reflectances", Journal of Graphics Tools 4(4), 1999, pp. 11-22 -- as
transcribed in wetplate, checked by its complement property.

**The light.** A Planckian radiator at 6504 K, the correlated colour
temperature of D65 (wetplate's approximation of daylight).

--------------------------------------------------------------------- method

    k_X = integral S(l) I(l) X(l) dl  /  integral S(l) I(l) W(l) dl

over 380..720 nm at 1 nm steps, S linearly interpolated between its 10 nm
points, X piecewise constant over Smits' bins.
"""
import argparse
import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADER = ROOT / "source" / "Spectral.h"

# Orthochromatic: an assumed shape (see above), relative, from 380 nm at 10 nm.
ORTHO_NM = 380
ORTHO = [
    0.80,  # 380
    0.90,  # 390
    0.97,  # 400
    1.00,  # 410
    1.00,  # 420
    0.98,  # 430
    0.92,  # 440
    0.80,  # 450
    0.62,  # 460
    0.45,  # 470
    0.32,  # 480
    0.25,  # 490
    0.24,  # 500
    0.30,  # 510
    0.42,  # 520
    0.58,  # 530
    0.70,  # 540
    0.74,  # 550
    0.70,  # 560
    0.55,  # 570
    0.32,  # 580
    0.12,  # 590
    0.03,  # 600
    0.00,  # 610
]

BASIS_START = 380.0
BASIS_END = 720.0
BASIS = {
    "white":   [1.0000, 1.0000, 0.9999, 0.9993, 0.9992, 0.9998, 1.0000, 1.0000, 1.0000, 1.0000],
    "cyan":    [0.9710, 0.9426, 1.0007, 1.0007, 1.0007, 1.0007, 0.1564, 0.0000, 0.0000, 0.0000],
    "magenta": [1.0000, 1.0000, 0.9685, 0.2229, 0.0000, 0.0458, 0.8369, 1.0000, 1.0000, 0.9959],
    "yellow":  [0.0001, 0.0000, 0.1088, 0.6651, 1.0000, 1.0000, 0.9996, 0.9586, 0.9685, 0.9840],
    "red":     [0.1012, 0.0515, 0.0000, 0.0000, 0.0000, 0.0000, 0.8325, 1.0149, 1.0149, 1.0149],
    "green":   [0.0000, 0.0000, 0.0273, 0.7937, 1.0000, 0.9418, 0.1719, 0.0000, 0.0000, 0.0025],
    "blue":    [1.0000, 1.0000, 0.8916, 0.3323, 0.0000, 0.0000, 0.0003, 0.0369, 0.0483, 0.0496],
}
ORDER = ["white", "cyan", "magenta", "yellow", "red", "green", "blue"]
KELVIN = 6504.0
REC709 = (0.2126, 0.7152, 0.0722)


def sensitivity(nm):
    x = (nm - ORTHO_NM) / 10.0
    if x <= 0.0:
        return ORTHO[0]
    i = int(math.floor(x))
    if i >= len(ORTHO) - 1:
        return ORTHO[-1]
    t = x - i
    return ORTHO[i] * (1.0 - t) + ORTHO[i + 1] * t


def basis(name, nm):
    width = (BASIS_END - BASIS_START) / 10.0
    i = int((nm - BASIS_START) / width)
    if nm >= BASIS_END:
        i = 9
    return BASIS[name][max(0, min(9, i))]


def planck(nm, kelvin):
    h = 6.62607015e-34
    c = 2.99792458e8
    k = 1.380649e-23
    lam = nm * 1e-9
    return (2.0 * h * c * c / lam ** 5) / (math.exp(h * c / (lam * k * kelvin)) - 1.0)


def ortho_weights():
    raw = {}
    for name in ORDER:
        total = 0.0
        for nm in range(int(BASIS_START), int(BASIS_END)):
            l = nm + 0.5
            total += sensitivity(l) * planck(l, KELVIN) * basis(name, l)
        raw[name] = total
    white = raw["white"]
    return {name: raw[name] / white for name in ORDER}


def pan_weights():
    r, g, b = REC709
    return {"white": 1.0, "cyan": g + b, "magenta": r + b, "yellow": r + g, "red": r, "green": g, "blue": b}


def complement_check():
    worst = 0.0
    for a, b in (("red", "cyan"), ("green", "magenta"), ("blue", "yellow")):
        for i in range(10):
            worst = max(worst, abs(BASIS[a][i] + BASIS[b][i] - BASIS["white"][i]))
    return worst


def literal(v):
    text = "%.9g" % v
    if "." not in text and "e" not in text:
        text += ".0"
    return text + "f"


def render():
    stocks = [("Orthochromatic", ortho_weights()), ("Panchromatic", pan_weights())]
    lines = [
        "#pragma once",
        "",
        "// GENERATED by tools/spectral_weights.py -- do not edit. Run the script.",
        "//",
        "// Each stock's exposure to each of Smits' seven basis reflectances in",
        "// daylight, white normalised to exactly 1. The shader sums these the way",
        "// Smits sums the spectra. Sources and method are in the script.",
        "",
        "namespace handleman::spectral",
        "{",
        "",
        "constexpr int kStockCount = %d;" % len(stocks),
        "",
        "/// Order: white, cyan, magenta, yellow, red, green, blue.",
        "struct Weights",
        "{",
        "\tconst char* stock;",
        "\tfloat w[ 7 ];",
        "};",
        "",
        "constexpr Weights kWeights[ kStockCount ] = {",
    ]
    for name, w in stocks:
        lines.append('\t{ "%s", { %s } },' % (name, ", ".join(literal(w[n]) for n in ORDER)))
    lines += [
        "};",
        "",
        "/// The orthochromatic sensitivity as committed, for the record: relative",
        "/// sensitivity from %d nm at 10 nm steps (an assumed shape)." % ORTHO_NM,
        "constexpr int kOrthoStartNm = %d;" % ORTHO_NM,
        "constexpr int kOrthoCount   = %d;" % len(ORTHO),
        "constexpr float kOrtho[ kOrthoCount ] = { %s };" % ", ".join("%.2ff" % s for s in ORTHO),
        "",
        "} // namespace handleman::spectral",
    ]
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="compare with the committed header, do not write")
    ap.add_argument("--print", action="store_true", help="print the weights")
    args = ap.parse_args()

    worst = complement_check()
    if worst > 0.08:
        print("the Smits table is mistranscribed: a complementary pair misses white by %.4f" % worst)
        return 1

    text = render()
    if args.print:
        for name, w in (("Orthochromatic", ortho_weights()), ("Panchromatic", pan_weights())):
            print("%-15s " % name + "  ".join("%s=%.5f" % (n[0].upper(), w[n]) for n in ORDER))
        print("complement check: worst %.4f" % worst)
    if args.check:
        if not HEADER.exists() or HEADER.read_text() != text:
            print("%s does not match what the script computes -- run tools/spectral_weights.py" % HEADER)
            return 1
        print("%s recomputes from the script's tables (complement check %.4f)" % (HEADER.relative_to(ROOT), worst))
        return 0
    HEADER.write_text(text)
    print("wrote %s" % HEADER.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
