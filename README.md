# Aloud

A minimal, battery-friendly audiobook player for jailbroken Kindles.

> **Status: pre-alpha — design and toolchain only, no app yet.** See [AGENTS.md](AGENTS.md)
> for the current state and roadmap.

## What it will do

- Play your own DRM-free audiobooks — M4B/M4A, MP3 (single files or chapter folders),
  OGG/Opus, FLAC — over Bluetooth (Paperwhite 4+, Oasis 2/3, Kindle 10th gen+, Scribe,
  Colorsoft) or the headphone jack (Kindle Touch).
- Chapters, resume exactly where you left off, 0.75–3× pitch-preserving speed, sleep timer.
- A clean e-ink interface designed for the screen: no animations, minimal refreshes.
- One small static binary that works on every firmware (soft- and hard-float).
- Runs as a scriptlet from your library, or from KUAL.

## Install (once released)

1. Jailbreak your Kindle and install the hotfix (see kindlemodding.org).
2. Unzip `aloud-<version>.zip` onto the Kindle's USB drive root.
3. Copy audiobooks into the `audiobooks` folder.
4. Open **Aloud** from your library.

## Building

See [AGENTS.md](AGENTS.md#setup--build-macos-linux-works-the-same-with-nproc).

## License

GPL-3.0-or-later. Uses [FBInk](https://github.com/NiLuJe/FBInk) (GPL-3.0+),
[FFmpeg](https://ffmpeg.org) (LGPL-2.1+), [sonic](https://github.com/waywardgeek/sonic)
(Apache-2.0) and [stb](https://github.com/nothings/stb) (public domain/MIT).
