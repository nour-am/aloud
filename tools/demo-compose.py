#!/usr/bin/env python3
"""Turn `aloud-sim --demo` output into an ffmpeg concat list for the README video.

Reads <demo>/timeline.txt (see src/demo.c), writes framed segment images and
<demo>/segments.txt (ffmpeg concat demuxer), and prints "<audio_start_ms> <end_ms>".
Each screen update becomes a segment lasting until the next one; a tap draws a
touch marker on the screen it lands on; a full (GC16) refresh starts with a short
inverted frame, the way an e-ink panel flashes.
"""
import os
import subprocess
import sys

FLASH_MS = 90
TAP_MS = 350

# Captions, switched at the scripted taps (src/demo.c order: book, play, speed,
# 1.5x, sleep, 30 min, chapters, chapter 2, pause): (first tap index, text).
CAPTIONS = [
    (None, "Your audiobooks, in any language"),
    (1, "Picks up right where you left off"),
    (2, "Up to 3\u00d7 faster, same voice"),
    (4, "Sleep timer with a gentle fade-out"),
    (6, "Jump to any chapter"),
    (8, "Aloud \u00b7 audiobooks for jailbroken Kindles"),
]


def run(*args):
    subprocess.run(args, check=True)


def main():
    demo, frame_sh, font = sys.argv[1], sys.argv[2], sys.argv[3]
    events = []
    with open(os.path.join(demo, "timeline.txt"), encoding="utf-8") as f:
        for line in f:
            p = line.split()
            if p:
                events.append(p)

    frames = [(int(p[1]), p[2], p[3]) for p in events if p[0] == "F"]
    taps = [(int(p[1]), int(p[2]), int(p[3])) for p in events if p[0] == "T"]
    audio_start = next((int(p[1]) for p in events if p[0] == "A"), 0)
    end = next(int(p[1]) for p in events if p[0] == "E")

    # Build a list of (start_ms, source_png, variant) cut points.
    cuts = []
    for i, (t, png, wfm) in enumerate(frames):
        if wfm == "gc16" and i > 0:
            cuts.append((t, png, "flash"))
            cuts.append((t + FLASH_MS, png, ""))
        else:
            cuts.append((t, png, ""))
    for t, x, y in taps:
        # The screen visible when the finger lands, with a marker, until the reaction.
        shown = max((c for c in cuts if c[0] <= t), key=lambda c: c[0])
        cuts.append((t, shown[1], "tap:%d:%d" % (x, y)))
        later = [c for c in cuts if t < c[0] <= t + TAP_MS]
        if not later:
            cuts.append((t + TAP_MS, shown[1], ""))
    # Caption changes are cuts too.
    switch = [(0 if i is None else taps[i][0], text) for i, text in CAPTIONS if i is None or i < len(taps)]

    def caption_at(t):
        return [text for start, text in switch if start <= t][-1]

    for start, _ in switch[1:]:
        shown = max((c for c in cuts if c[0] <= start), key=lambda c: c[0])
        if shown[0] != start:
            cuts.append((start, shown[1], shown[2]))
    cuts.sort(key=lambda c: c[0])

    seg_dir = os.path.join(demo, "segments")
    os.makedirs(seg_dir, exist_ok=True)
    made = {}
    lines = ["ffconcat version 1.0"]
    last = None
    for i, (t, png, variant) in enumerate(cuts):
        nxt = cuts[i + 1][0] if i + 1 < len(cuts) else end
        dur = nxt - t
        if dur <= 0:
            continue
        caption = caption_at(t)
        key = (png, variant, caption)
        if key not in made:
            out = os.path.join(seg_dir, "s%04d.png" % len(made))
            src = os.path.join(demo, png)
            run("sh", frame_sh, src, out, variant or "plain", caption, font)
            made[key] = out
        last = os.path.abspath(made[key])
        lines.append("file '%s'" % last)
        lines.append("duration %.3f" % (dur / 1000.0))
    lines.append("file '%s'" % last)  # concat demuxer needs the last file repeated
    with open(os.path.join(demo, "segments.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(audio_start, end)


if __name__ == "__main__":
    main()
