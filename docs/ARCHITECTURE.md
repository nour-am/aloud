# Aloud — Architecture & decisions

Each decision lists the alternatives considered so a later contributor can revisit it with
the same information. Facts about the Kindle platform (with sources) live in
[RESEARCH.md](RESEARCH.md); this file only records what we decided and why.

## D1. Native C11 binary, no runtime

**Decision:** one C11 program (`aloud`), launched from a shell script by KUAL.

**Why:** lowest CPU, memory and startup cost on 256–512 MB, 1 GHz single/dual-core
Kindles; no interpreter to ship; direct access to the framebuffer, evdev and ALSA.

**Rejected:** KOReader plugin (not standalone — owner requirement); GTK2 app on the stock
X server (less control over e-ink refresh = more refreshes, and depends on firmware libs);
HTML "illusion"/WAF app (no reliable audio API in the Kindle browser engine).

## D2. Fully static ARMv7 musl binary, built with Zig from macOS

**Decision:** compile everything (app + FFmpeg + FBInk + sonic + stb) into one static
executable with `zig cc -target arm-linux-musleabihf -mcpu=cortex_a8`
(wrapper: `tools/arm-cc`).

**Why:**
- Kindle firmware ≥ 5.16.3 switched userland to hard-float (armhf); older firmware is
  soft-float (armel) — see RESEARCH.md. A *static* binary carries its own libc and float
  calling convention, so **one binary runs on both** and on every glibc version. This
  removes an entire class of "works on my Kindle, not yours" failures.
- Cortex-A8 tuning (ARMv7-A, VFPv3-D32, NEON) is the common baseline of every
  audio-capable Kindle we target (i.MX50/6/7 Cortex-A8/A9/A7, MediaTek Cortex-A7/A53 in
  32-bit mode).
- Zig ships the cross linker and musl, so no Docker/crosstool-ng is needed on the dev Mac
  (none is installed). Verified: FFmpeg 9.0.2 cross-builds with NEON+VFP enabled, and a
  static ARM test binary linking all decoders is 1.6 MB stripped.

**Gotcha:** zig parses `-mcpu=cortex-a8` as CPU `cortex` minus feature `a8`. The wrapper
drops any `-mcpu/-march/-mtune/-mfpu/-mfloat-abi` passed by third-party build systems and
pins `-mcpu=cortex_a8` itself.

**Constraint this creates:** a static musl binary **cannot dlopen the device's glibc
libraries** (e.g. `libasound.so.2` with its plugins, `liblipc.so`). Anything we need from
the firmware must be reached through the kernel (ioctl), a socket/D-Bus protocol, or by
spawning a firmware binary (`lipc-set-prop`, `lipc-wait-event`, …). Bluetooth audio does
require a firmware library (GStreamer `mixersink`), so D6 adds one tiny dynamically linked
helper, built twice (zig `arm-linux-gnueabi.2.12` and `arm-linux-gnueabihf.2.20`, both
verified to compile), that receives PCM over a pipe.

## D3. Audio decoding: minimal static FFmpeg (LGPL parts only)

**Decision:** FFmpeg 9.0.2 built by `scripts/build-ffmpeg.sh` with `--disable-everything`
and only: demuxers mov, mp3, ogg, flac, wav, aac, matroska; decoders aac, mp3float, mp3,
opus, vorbis, flac, alac, pcm_*; parsers aac, mpegaudio, flac, opus, vorbis; protocol file;
libswresample. No video, filters, network, autodetected externals or GPL components.

**Why:** battle-tested handling of real-world files (VBR MP3 seeking, broken M4B atoms,
both M4B chapter flavours, ID3 CHAP, Opus/Vorbis), accurate seeking, chapters and tags
for free, NEON-optimised decoders. Decode cost for speech AAC/MP3 is ~1–3 % of one core.

**Rejected:** minimp3 + faad2 + hand-written MP4 parser (smaller, but every malformed file
in the wild becomes our bug); device GStreamer (plugin set differs per firmware).

## D4. Speed control: sonic

**Decision:** [sonic](https://github.com/waywardgeek/sonic) (Apache-2.0, single C file) for
pitch-preserving 0.5–3.0× playback, applied after resampling to the output format.

**Why:** designed for speech, far cheaper than FFmpeg's `atempo` (which would also pull in
libavfilter). Also provides volume scaling if we need software volume.

## D5. Display: own 8-bit canvas, FBInk for the e-ink plumbing

**Decision:**
- UI is drawn into an in-memory 8-bit grayscale canvas by our own small renderer
  (rects, hairlines, rounded rects, anti-aliased vector icons, text).
- Text via `stb_truetype` with a glyph cache; icons are vector paths rasterised with
  `stbtt_Rasterize` (stb's AA scanline rasteriser), so no icon font or image assets.
- Covers decoded with `stb_image` (JPEG/PNG), downscaled with a box filter and
  Floyd–Steinberg-dithered to the 16-level e-ink palette; thumbnails cached on disk.
- Dirty rectangles are pushed to the framebuffer through FBInk and refreshed with an
  explicit waveform per DESIGN.md's refresh policy.
- FBInk is compiled into the binary from source with
  `-DFBINK_FOR_KINDLE -DFBINK_MINIMAL -DFBINK_WITH_INPUT` (+ `-DFBINK_WITH_IMAGE` if we use
  `fbink_print_raw_data` for the blit). Verified to compile with `tools/arm-cc`. Its
  Makefile is not used (gcc-ar defaults, i2c-tools dependency).
- Covers are dithered in software: hardware dithering is broken on PW5/KT per KOReader.
- Waveforms: DU for tiny B/W changes, GL16 for gray partials, GC16 flashing for full
  repaints. Avoid REAGL non-flashing on Kindle (driver expects REAGL to flash). On MTK,
  check whether fast mode must be toggled to stop the driver promoting refreshes to REAGL.

**Why:** FBInk (GPL-3.0-or-later, by NiLuJe, used by KOReader/KFMon) already handles
every Kindle EPDC generation (i.MX `mxcfb`, MediaTek `hwtcon`), device identification,
DPI, inverted/8/32 bpp framebuffers and touch-axis quirks. Owning the canvas keeps the UI
pixel-identical between the Kindle and the host simulator (so screenshots in tests are
exactly what the device shows) and lets us refresh precise regions.

**Licence consequence:** FBInk is GPL-3.0-or-later → Aloud is **GPL-3.0-or-later**
(LICENSE). FFmpeg (LGPL-2.1+), sonic (Apache-2.0) and stb (public domain/MIT) are
compatible. Bundled fonts must be OFL.

## D6. Audio output: PCM pipe into the firmware's GStreamer `mixersink`

**Facts driving this (RESEARCH.md §6):** Bluetooth Kindles have **no ALSA sound card**.
The only working route to Bluetooth A2DP is Lab126's proprietary GStreamer element
`mixersink` (GStreamer 0.10 before FW 5.19, 1.0 from 5.19). Every confirmed recipe feeds it
**S16LE, 44 100 Hz, stereo**. The Kindle Touch instead has a plain ALSA hardware card.

**Decision — two backends behind `audio_out.h`, chosen at runtime:**

1. **`gst` backend (all Bluetooth Kindles).** The static app spawns a tiny helper,
   `aloud-sink`, and writes PCM to its stdin (a pipe; no FIFO on vfat). The helper is the
   *only* dynamically linked code in the project: plain C, ~200 lines, it
   `dlopen()`s `libgstreamer-1.0.so.0` or `libgstreamer-0.10.so.0` (whichever exists),
   builds `fdsrc fd=0 ! <caps> ! mixersink` with the matching caps string (see
   RESEARCH.md §6.3), and accepts PLAY/PAUSE/STOP + position queries on a control pipe.
   It is built twice with Zig — `arm-linux-gnueabi.2.12` (armel, FW < 5.16.3) and
   `arm-linux-gnueabihf.2.20` (armhf) — no GStreamer headers or libs needed at build
   time (we declare the dozen function pointers we use). The launcher picks the build by
   checking `/lib/ld-linux-armhf.so.3` (KOReader's test).
   - Fallback if the helper can't start: spawn the firmware's own `gst-launch-1.0` /
     `gst-launch-0.10` CLI with the same pipeline, if present.
   - Why a helper instead of making the whole app dynamic: keeps FFmpeg, FBInk and all our
     logic in one static binary immune to glibc/float-ABI differences; only the
     GStreamer handshake depends on firmware libraries.
   - Why `fdsrc` over KinAMP's `filesrc` on a FIFO: no filesystem object to clean up after
     a crash. (If `fdsrc` misbehaves with `mixersink` on device, switch to a FIFO in
     `/tmp` exactly like KinAMP.)
2. **`alsa` backend (Kindle Touch, any device with `/proc/asound/cards` listing a card).**
   Raw kernel ALSA (`/dev/snd/pcmC0D0p` ioctls, tinyalsa-style) from the static binary —
   no libasound needed.

**Output format:** always resample to S16LE 44.1 kHz stereo (libswresample), then sonic
(speed), then software volume. Mono sources are duplicated to both channels.

**Behaviour rules:**
- Pause = PAUSE the helper's pipeline (instant, no latency); after 2 min paused, stop the
  helper entirely so the mixer/Bluetooth path can idle. Resume rewinds 2 s (standard
  audiobook behaviour, also absorbs pipe-latency position error).
- Position comes from the helper's pipeline clock when available, else from
  bytes-written minus a measured pipe+sink latency.
- **Volume is software** (no mixer on BT models); the UI says the system level is set in
  Settings › Accessibility › VoiceView › Volume.
- If no Bluetooth device is connected (`lipc-get-prop com.lab126.btfd BTstate` /
  `ListConnected`), show "Connect headphones in Settings › Bluetooth" and don't start
  the pipeline. After a pause longer than ~20 min, BT may have idle-disconnected: try
  `lipc-set-prop com.lab126.btfd Connect <last MAC>` once before showing the message.
  Do **not** cycle `BTenable` (drops headphones for seconds) unless the user asks.

## D12. Power while playing: let the screensaver happen, defer only the suspend

Holding `preventScreenSaver 1` (what KinAMP/LARK do) keeps the device fully awake with the
frontlight on and reportedly swallows short power-button presses. Aloud instead:
- While **playing**: do nothing until powerd emits `readyToSuspend`, then answer with
  `lipc-set-prop com.lab126.powerd deferSuspend <seconds>` (only valid in that state),
  sized to the sleep timer or a long default; re-arm on every `readyToSuspend`.
- While **paused** (or when the sleep timer fires): don't defer, so the Kindle suspends
  normally and the battery is spared.
- Power events are read from a long-lived child `lipc-wait-event -m -s 0
  com.lab126.powerd goingToScreenSaver,outOfScreenSaver,exitingScreenSaver,readyToSuspend,wakeupFromSuspend,suspending`
  whose stdout is one of the main loop's poll fds.
- `goingToScreenSaver` → stop all drawing; `outOfScreenSaver` → one full GC16 repaint.
- **Unverified on current firmware** (RESEARCH.md §6 Unknown #3, #7). If deferSuspend
  doesn't hold audio on some model, the fallback is `preventScreenSaver 1` while playing
  plus our own "screen off" (frontlight to 0 and a static now-playing screen), restored on
  any touch. This is the first thing to test on hardware.

## D7. Threads and event loop

- **Main thread:** `poll()` on the touch/button evdev fds, a self-pipe for audio-thread
  notifications, and a single timer for the next UI tick (minute tick, sleep timer,
  autosave). When paused and idle, `poll()` has **no timeout** → zero wakeups.
- **Audio thread:** demux → decode → resample → sonic → blocking write to the output.
  Commands (play, pause, seek, set speed, load book) arrive through a mutex-protected
  command slot + condition variable; it reports position/state back through the self-pipe.
- Position = frames handed to the output minus the output's reported delay, converted to
  book time (accounting for speed).

## D8. Persistence — never lose the position

- State dir: `/mnt/us/extensions/aloud/state/` (inside the extension so uninstall cleans
  up; the zip doesn't contain it so updates keep it).
- `positions.tsv`: one line per book: `key  file_index  position_ms  speed  updated_epoch`.
  Key = path relative to the library root.
- Written atomically: write `positions.tsv.tmp` → `fsync` → `rename` → `fsync(dir)`.
  On load, a missing/corrupt file falls back to `positions.tsv.bak` (previous good copy).
- Save triggers: pause, seek, chapter change, book switch, sleep-timer stop, exit/signal,
  and every 60 s while playing. Never more often (flash wear, wakeups).
- Library cache (`library.tsv`) keyed by path + size + mtime so FFmpeg probing happens
  only for new/changed files; thumbnails as raw 8-bit files named by a hash of the key.

## D9. Packaging, launch & crash safety

**Packaging:** one zip unzipped onto the Kindle's USB root (layout in RESEARCH.md §2).
Entry points: a **scriptlet** `documents/Aloud.sh` (`# DontUseFBInk`) — primary, because
classic KUAL is dead on FW ≥ 5.19.4 — plus `extensions/aloud/{config.xml,menu.json}` for
KUAL/KUALA/KUAL Next users. A KPM package later. No MRPI `.bin` (we never write outside
`/mnt/us`).

**Launcher** (`extensions/aloud/aloud.sh`), modelled on KOReader's "framework kept
running" mode (RESEARCH.md §3) because the stock power and Bluetooth services must stay
alive for audio:
1. Re-exec from `/var/tmp`; single-instance lock.
2. Dump `/dev/fb0` for restore; `disableEnablePillow disable`; `killall -STOP awesome`;
   `stop statusbar` if that job exists; `killall -STOP volumd` (USB mass storage would yank
   the files we're playing).
3. Run `bin/aloud`.
4. **Always** (in a `trap … EXIT INT TERM HUP`, so also after a crash): `killall -CONT
   volumd awesome`, `start statusbar` if stopped, restore fb dump, `disableEnablePillow
   enable`, `lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home`,
   undo any `preventScreenSaver`.

The binary itself grabs its input devices (`EVIOCGRAB`) and handles SIGTERM/SIGINT/SIGHUP
by saving state and exiting cleanly.

## D11. Orientation & touch mapping

- **Never change the framebuffer rotation** (the stock X server is only paused, and it
  breaks if the fb is rotated under it). Layout is computed from the current
  `view_width × view_height`; portrait is the designed layout, landscape (Oasis/Scribe held
  sideways) gets a two-column variant (cover left, controls right).
- Touch → screen: read `EVIOCGABS` min/max for X/Y, normalise to 0..1, swap axes when the
  raw aspect ratio disagrees with the view's, then apply FBInk's `rotation_map`
  (KUAL Next's approach). FBInk has no swap/mirror quirks for Kindles.
- Accept both multitouch protocols; a contact ends on `ABS_MT_TRACKING_ID -1` **or**
  `BTN_TOUCH 0` (KT4 never sends the former).
- Page-turn keys (Oasis 104/109, reversed vs Voyage) → previous/next page in lists,
  skip back/forward in the player.

## D10. Testing strategy

- **Host simulator build** (`make sim`, macOS/Linux, native clang): same code with a
  `platform_sim.c` backend — the canvas is written to PNG files, input comes from a
  scripted event list, audio goes to a WAV file or null sink. Every screen gets a golden
  screenshot test.
- **Unit tests** (`make test`): library discovery & natural sort, metadata/chapter
  extraction, position math across multi-file books, speed changes, seek clamping,
  sleep timer, persistence (atomic write, corrupt-file recovery), touch decoding for both
  multitouch protocol A and B and every axis-swap/mirror quirk, hit-testing.
- **Fixtures** are generated at test time with the host `ffmpeg` (sine tones with
  chapters/tags/cover) — no binary blobs in git.
- **ARM binary checks:** the static ARM build is inspected (`file`, size budget) in CI; it
  cannot be executed on the Mac (no qemu-user on macOS).
- **On-device checklist** (`docs/DEVICE-TESTING.md`, to be written) for what only real
  hardware can verify: audio path per model, refresh quality, suspend behaviour, battery
  drain per hour.
