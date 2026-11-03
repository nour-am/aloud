# AGENTS.md — handoff for any agent continuing Aloud

Read this first, then `docs/ARCHITECTURE.md` (decisions), `docs/RESEARCH.md` (Kindle
platform facts with sources) and `docs/DESIGN.md` (UI spec). Those four files are the
complete project context; nothing important lives only in a chat transcript.

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
  Bluetooth, power (docs/RESEARCH.md §1–6).
- Architecture decided (docs/ARCHITECTURE.md D1–D12). Key calls:
  static musl app (D2) + tiny dynamic `aloud-sink` helper feeding the firmware's GStreamer
  `mixersink`, because **Bluetooth Kindles have no ALSA card** (D6); raw ALSA for the
  Kindle Touch; screensaver allowed while playing, only suspend deferred (D12);
  scriptlet as primary launcher because KUAL is dead on FW ≥ 5.19.4 (D9).
- UI/product spec (docs/DESIGN.md).
- Toolchain proven on the owner's Mac: Zig 0.17 cross-compiles static ARMv7 binaries;
  FBInk compiles for ARM; minimal FFmpeg 9.0.2 builds for **host and Kindle** (NEON+VFP);
  a static ARM test binary linking all decoders is 1.6 MB stripped; the glibc helper
  targets (`arm-linux-gnueabi.2.12` soft-float, `arm-linux-gnueabihf.2.20` hard-float)
  build with `dlopen` and carry the right ELF float-ABI flags.
- Pinned dependency fetching (`scripts/fetch-deps.sh`, commit hashes + SHA-256).
- `tools/kindle-diagnose.sh`: read-only on-device fact finder (masks serial/MACs);
  dry-run tested on macOS only.

Not started: all application code (no `src/` yet), tests, Makefile, launcher, packaging.
Not chosen yet: bundled fonts (need OFL serif + sans with *static* TTFs, since stb_truetype
ignores variable-font axes; candidates Literata or Source Serif 4 + Inter).

Biggest risks (all need real hardware, see RESEARCH.md §6 "Unknown"): `mixersink` on
FW ≥ 5.19 (GStreamer 1.0), whether `deferSuspend` keeps audio alive in screensaver,
Broadcom models dropping BT in screensaver.

Closest existing project: **LARK** (https://github.com/kbarni/LARKPlayer) — an M4B/MP3
audiobook player for Kindle by KinAMP's author, FW < 5.19 only. Its/KinAMP's source is the
best reference for the audio path.

## Repo map

```
AGENTS.md            this file (CLAUDE.md imports it)
README.md            user-facing summary
LICENSE              GPL-3.0 (forced by FBInk's GPL-3.0-or-later)
docs/ARCHITECTURE.md decisions D1–D12 with rejected alternatives
docs/RESEARCH.md     Kindle facts + sources + unknowns; local env notes (§7)
docs/DESIGN.md       screens, refresh policy, content layout, discovery rules
scripts/fetch-deps.sh   clone/download pinned third-party sources into third_party/
scripts/build-ffmpeg.sh build minimal static FFmpeg: `host` or `kindle`
tools/arm-cc         zig cc wrapper: arm-linux-musleabihf, cortex_a8 (drops GCC -mcpu etc.)
tools/arm-ar, tools/arm-ranlib
tools/kindle-diagnose.sh read-only on-device fact finder (scriptlet), writes aloud-diagnose.txt
src/ tests/ package/ helper/   not created yet (planned layout below)
third_party/  build/   git-ignored, generated
```

## Setup & build (macOS; Linux works the same with nproc)

```sh
brew install zig            # 0.17.0 verified; Homebrew ffmpeg is used for test fixtures
scripts/fetch-deps.sh       # FBInk, sonic, stb (pinned commits), FFmpeg 9.0.2 (sha256)
scripts/build-ffmpeg.sh host     # → build/host/ffmpeg/{include,lib}
scripts/build-ffmpeg.sh kindle   # → build/kindle/ffmpeg/{include,lib}  (~3 min)
```
On the owner's Mac all four steps have already been run; `third_party/` and `build/`
exist locally (git-ignored).

Linking against the result (verified):
```sh
# Kindle (static, stripped)
tools/arm-cc -O2 -static -s -Ibuild/kindle/ffmpeg/include x.c -Lbuild/kindle/ffmpeg/lib \
  -lavformat -lavcodec -lswresample -lavutil -lm -Wl,--gc-sections -o x
# Host
clang -O2 -Ibuild/host/ffmpeg/include x.c -Lbuild/host/ffmpeg/lib \
  -lavformat -lavcodec -lswresample -lavutil -lm -framework CoreFoundation -o x
```
FBInk for Kindle: compile `fbink.c cutef8/utf8.c cutef8/dfa.c fbink_input_scan.c` with
`-std=gnu11 -D_GNU_SOURCE -DFBINK_FOR_KINDLE -DFBINK_MINIMAL -DFBINK_WITH_INPUT
-DFBINK_VERSION='"<rev>"'` (add `-DFBINK_WITH_IMAGE` + `qimagescale/qimagescale.c` if using
`fbink_print_raw_data`). Don't use FBInk's Makefile (see ARCHITECTURE.md D5).

## Planned source layout

```
src/main.c              lifecycle + poll() event loop (zero wakeups when paused & idle)
src/platform.h          display (size, dpi, blit+refresh w/ waveform), input events, power hooks
src/platform_kindle.c   FBInk + evdev (+EVIOCGRAB) + lipc via spawned lipc-* binaries
src/platform_sim.c      host: canvas → PNG, scripted input, for tests and screenshots
src/audio_out.h         output interface; backends: gst helper, raw ALSA, WAV/null (host)
src/audio_out_gst.c     spawns aloud-sink, writes S16LE 44.1k stereo to its stdin
src/audio_out_alsa.c    raw kernel ALSA for the Kindle Touch
helper/aloud_sink.c     the only dynamic binary: dlopen libgstreamer, fdsrc ! caps ! mixersink
src/power.[ch]          lipc-wait-event child, deferSuspend on readyToSuspend (D12)
src/decoder.[ch]        FFmpeg: open, PCM out, seek, tags, chapters, cover bytes
src/player.[ch]         audio thread, commands, position, sonic speed, sleep timer, fade
src/library.[ch]        discovery rules (DESIGN.md), natural sort, metadata cache
src/store.[ch]          positions.tsv atomic persistence + .bak recovery (D8)
src/canvas.[ch]         8-bit canvas: rects, hairlines, rounded rects, blits, dirty rects
src/text.[ch]           stb_truetype fonts, glyph cache, measure, wrap, ellipsis, tabular figs
src/icons.[ch]          vector icons via stbtt_Rasterize
src/ui_*.c              library, player, chapters, sheets, messages
tests/                  unit tests + golden screenshots; fixtures generated with ffmpeg
package/                aloud.sh launcher, scriptlet, config.xml, menu.json, README.txt
```

## Roadmap (do in order; each step ends with tests passing and a commit)

1. **Get device facts.** Ask the owner to copy `tools/kindle-diagnose.sh` into the
   Kindle's `documents/` folder, open it from the library, and share
   `aloud-diagnose.txt` from the USB root. Record anything new in RESEARCH.md (and update
   D6/D12 if a fact contradicts them). Then build a throwaway `aloud-sink` + a static
   "play a 440 Hz tone for 10 s" test binary so the owner can confirm sound on their
   device before the UI exists. Ask them to test: tone plays; tone keeps playing after
   pressing power (screensaver); and whether `deferSuspend` in `readyToSuspend` holds it.
2. **Engine on host:** Makefile (`make test`, `make sim`, `make kindle`), decoder, store,
   library, player (with a WAV/null output), plus unit tests (list in ARCHITECTURE.md D10).
   Fixtures: generate with Homebrew ffmpeg at test time (sine tones; M4B with chapters via
   ffmetadata; MP3 folder with ID3 tags; cover art; a deliberately truncated file).
3. **UI in the simulator:** canvas, text, icons, screens per DESIGN.md; golden PNGs in
   `tests/golden/` at PW5 (1236×1648 @300) and KT4 (600×800 @167) sizes, plus landscape.
   Commit screenshots to `docs/screens/` for the owner to review the design.
4. **Kindle platform layer** (FBInk display, evdev input with protocol A/B + KT4 quirk,
   lipc power events via `lipc-wait-event` child process), launcher with crash-safe restore,
   `make package` → `dist/aloud-<version>.zip`.
5. **Device testing** with the owner: write `docs/DEVICE-TESTING.md` checklist (audio
   per model, suspend/screensaver while playing, refresh quality, crash-restore, battery %
   per hour playing vs. stock Audible).
6. **Release:** README with screenshots + install steps, GitHub release, MobileRead thread,
   KPM package.

## Conventions

- C11, `-Wall -Wextra -Werror` on our code (not third_party). snake_case; module prefixes
  (`ui_`, `audio_`, `lib_`, `store_`). Small files, one responsibility each.
- Check every syscall/FFmpeg return; never `assert` on user data — degrade with a message.
- No polling loops, no per-second timers, no allocation in the audio hot path.
- Comments explain *why* (hardware quirks, cited sources), not *what*.
- Never commit third_party/ or build/. Add new deps to `scripts/fetch-deps.sh`, pinned.
- Licences: GPL-3.0-or-later overall; only add GPL-3-compatible deps; fonts must be OFL.
- Commit messages: imperative summary line; end with the attribution trailer the session
  requires.

## Open questions for the owner

- Which Kindle model(s) and firmware do they have for testing? (Bluetooth model vs Kindle
  Touch decides which audio path is tested first.)
- Name: working title "Aloud" — confirm or rename (only README, scripts and docs use it).
- Default library folder `/mnt/us/audiobooks` — OK?
