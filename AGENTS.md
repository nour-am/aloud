# AGENTS.md — handoff for any agent continuing Aloud

Read this first, then `docs/ARCHITECTURE.md` (decisions), `docs/RESEARCH.md` (Kindle
platform facts with sources), `docs/DESIGN.md` (UI spec), and `docs/DEVICE-TESTING.md`
(on-device verification checklist).

## Mission

**Aloud** is a standalone audiobook player for jailbroken Amazon Kindles (FW 5.x), meant to
become *the* audiobook app for the Kindle modding community (distributed as a zip on
GitHub/MobileRead). The owner's priorities, in order:

1. **Reliability** — tested, "fail-proof": never lose a position, never crash on odd files,
   always give the Kindle back in a working state.
2. **Efficiency** — least possible battery and CPU/RAM on weak Kindle hardware.
3. **Pristine, minimal design** native to e-ink.

It must be standalone (not a KOReader plugin). The owner also maintains `../libro`, a CLI
that downloads audiobooks as tagged chapter MP3 folders or single M4B files with chapters —
those two shapes are the primary content Aloud must handle perfectly.

## Status (2026-11-03)

Done:
- Platform research, sourced: jailbreaks, packaging, launching, display, input, audio,
  Bluetooth, power (`docs/RESEARCH.md` §1–6).
- Architecture decided (`docs/ARCHITECTURE.md` D1–D12).
- UI/product spec (`docs/DESIGN.md`).
- Pinned dependency fetching (`scripts/fetch-deps.sh`, commit hashes + SHA-256), including
  static OFL TTF fonts (`Literata-Regular.ttf`, `Literata-SemiBold.ttf`, `Inter-Regular.ttf`,
  `Inter-SemiBold.ttf`).
- Complete C11 implementation (`src/`, `helper/`):
  - `src/store.[ch]`: atomic `positions.tsv` persistence with `# aloud-positions v1` / `# end`
    sentinel and `.bak` recovery.
  - `src/decoder.[ch]`: FFmpeg 9.0.2 demux/decode/resample to 44.1 kHz S16LE stereo, sample
    seeking, chapter & tag metadata, embedded cover extraction, corrupt/truncated file survival.
  - `src/library.[ch]`: recursive discovery of single-file books and multi-file folders,
    natural sort (`"2" < "10"`), metadata cache (`library.tsv`), dithered cover thumbnail cache.
  - `src/audio_out*.[ch]` & `helper/aloud_sink.c`: host WAV/null sink, Kindle Touch raw kernel
    ALSA ioctl sink, and Bluetooth GStreamer `mixersink` helper (`aloud-sink-armel` +
    `aloud-sink-armhf` with `dlopen` for GStreamer 1.0 & 0.10 and `fdsrc` / `/dev/stdin` fallback).
  - `src/power.[ch]`: `lipc-wait-event -s 0 com.lab126.powerd` monitor, `deferSuspend` on
    `readyToSuspend` while playing, screensaver tracking.
  - `src/player.[ch]`: audio thread + deterministic step API, `sonic` pitch-preserving speed
    (`0.75×`–`3.0×`), software volume, 2 s resume rewind, 2-min idle sink close, sleep timer
    with 10 s linear fade-out and `End of chapter` mode.
  - `src/canvas.[ch]`, `src/text.[ch]`, `src/icons.[ch]`, `src/ui.[ch]`: 8-bit grayscale e-ink
    renderer, Floyd–Steinberg 16-level cover dithering, SFNT validator + `stb_truetype` glyph
    LRU cache, `stbtt_Rasterize` vector icons, physical-mm layout for 300 DPI / 167 DPI /
    Landscape, surgical `DU`/`GL16`/`GC16` refresh regions.
  - `src/platform_kindle.c`, `src/platform_sim.c`, `src/main.c`: FBInk + evdev (`EVIOCGRAB`,
    Protocol A & B, KT4 `BTN_TOUCH 0` quirk, axis swap, rotation map, Oasis/Voyage page keys) +
    `poll()` loop (`-1` timeout when paused & idle).
- 185-check unit & integration test suite (`make test`) + golden screenshot generator
  (`make sim` → `docs/screens/*.png`).
- Crash-safe Kindle launcher & packaging (`make package` → `dist/aloud-0.1.0.zip`, 1.8 MB) +
  on-device diagnostic & audio smoke-test scripts (`tools/kindle-diagnose.sh`,
  `tools/kindle-audio-test.sh`, `docs/DEVICE-TESTING.md`).

Owner feedback round 1 (stacked local branches, one per item, all tests green):
`fix/progress-1s-sync` (#1) → `fix/immediate-pause` (#2) → `fix/fast-scrubber-seek` (#3) →
`fix/playback-start-glitch` (#4) → `fix/status-bar-overlap` (#5) → `fix/position-autosave` (#6).
Items #3–#6 need confirming on the device (see `docs/DEVICE-TESTING.md`).

Next step (requires physical Kindle hardware):
- Run through `docs/DEVICE-TESTING.md` on the owner's Kindle(s) and record findings from
  `aloud-diagnose.txt` and `aloud-audio-test.txt` in `docs/RESEARCH.md`.

## Repo map

```
AGENTS.md               this file (CLAUDE.md imports it)
README.md               user-facing overview, screenshots, install & build guide
LICENSE                 GPL-3.0-or-later
Makefile                build targets: test, sim, kindle, package, clean
docs/ARCHITECTURE.md    decisions D1–D12 with rejected alternatives
docs/RESEARCH.md        Kindle facts + sources + unknowns; local env notes (§7)
docs/DESIGN.md          screens, refresh policy, content layout, discovery rules
docs/DEVICE-TESTING.md  on-device verification checklist & troubleshooting
docs/screens/           generated UI screenshots (PW5, KT4, Landscape)
scripts/fetch-deps.sh   clone/download pinned third-party sources + OFL fonts into third_party/
scripts/build-ffmpeg.sh build minimal static FFmpeg: `host` or `kindle`
tools/arm-cc            zig cc wrapper: arm-linux-musleabihf, cortex_a8
tools/arm-ar, tools/arm-ranlib
tools/kindle-diagnose.sh   read-only on-device fact finder (scriptlet)
tools/kindle-audio-test.sh standalone 10 s 440 Hz tone smoke-test scriptlet
helper/aloud_sink.c     dynamic glibc helper: dlopen libgstreamer-1.0/0.10, fdsrc ! mixersink
src/                    C11 application modules (store, decoder, library, player, power, ui, platform)
tests/                  gen_fixtures.sh + test_main.c (185 checks)
package/                launcher.sh, scriptlet.sh, config.xml, menu.json, audiobooks-README.txt
third_party/ build/ dist/  git-ignored, generated
```

## Setup & build (macOS; Linux works the same with nproc)

```sh
brew install zig            # 0.17.0 verified; Homebrew ffmpeg is used for test fixtures
./scripts/fetch-deps.sh     # FBInk, sonic, stb, FFmpeg 9.0.2, Literata 3.103, Inter 4.1
./scripts/build-ffmpeg.sh host     # → build/host/ffmpeg/{include,lib}
./scripts/build-ffmpeg.sh kindle   # → build/kindle/ffmpeg/{include,lib}  (~3 min)
make test                   # runs 185 unit & integration checks
make sim                    # exports screenshots into docs/screens/
make package                # builds Kindle binaries and creates dist/aloud-0.1.0.zip
```

## Conventions

- C11, `-Wall -Wextra -Werror` on our code (not third_party). snake_case; module prefixes
  (`ui_`, `audio_`, `lib_`, `store_`). Small files, one responsibility each.
- Check every syscall/FFmpeg return; never `assert` on user data — degrade with a message.
- No polling loops, no per-second timers, no allocation in the audio hot path.
- Comments explain *why* (hardware quirks, cited sources), not *what*.
- Never commit `third_party/`, `build/`, or `dist/`. Add new deps to `scripts/fetch-deps.sh`, pinned.
- Licences: GPL-3.0-or-later overall; only add GPL-3-compatible deps; fonts must be OFL.
