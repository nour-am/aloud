#!/usr/bin/env python3
"""Print the code points Aloud keeps from Noto Sans CJK, as pyftsubset --unicodes.

The full font is 16 MB per weight. Book titles, authors and chapter names only
need everyday characters, so we keep the national standard sets:
  - GB 2312        (Simplified Chinese, 6,763 hanzi)
  - JIS X 0208     (Japanese level 1+2 kanji, kana, symbols)
  - Big5 level 1   (5,401 common Traditional Chinese hanzi)
plus CJK punctuation, kana blocks and full-width forms. A character outside
these sets falls back to "?" like any other missing glyph.
"""
import sys


def decode_range(codec, lead, trail):
    out = set()
    for a in lead:
        for b in trail:
            try:
                s = bytes([a, b]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(s) == 1:
                out.add(ord(s))
    return out


def main():
    cps = set()
    cps |= decode_range("gb2312", range(0xA1, 0xF8), range(0xA1, 0xFF))
    cps |= decode_range("euc_jp", range(0xA1, 0xF5), range(0xA1, 0xFF))
    # Big5 level 1 hanzi (A440-C67E) and symbols (A140-A3BF).
    cps |= decode_range("big5", range(0xA1, 0xC7),
                        list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)))
    for lo, hi in [
        (0x3000, 0x303F),  # CJK symbols and punctuation
        (0x3040, 0x30FF),  # hiragana, katakana
        (0x31F0, 0x31FF),  # katakana phonetic extensions
        (0xFF00, 0xFFEF),  # half-width and full-width forms
    ]:
        cps |= set(range(lo, hi + 1))
    # Only CJK: Latin, Greek and Cyrillic come from the primary fonts.
    cps = {c for c in cps if c >= 0x2E80}
    sys.stdout.write(",".join("U+%04X" % c for c in sorted(cps)) + "\n")
    sys.stderr.write("%d code points\n" % len(cps))


if __name__ == "__main__":
    main()
