# Contributing to Aloud

Thanks for helping. Aloud is a small project, so focused changes are the easiest to review
and the most likely to be merged.

## Ways to help

- **Device reports.** Aloud is tested on a Paperwhite 5. If you have another Kindle, a
  [device report](https://github.com/nour-am/aloud/issues/new?template=device_report.yml)
  tells us what works on it, even if everything does.
- **Bug reports.** Use the [bug template](https://github.com/nour-am/aloud/issues/new?template=bug_report.yml)
  and include your model, firmware and steps to reproduce.
- **Ideas.** Start in [Discussions](https://github.com/nour-am/aloud/discussions)
  before opening a feature request, so we can agree on the shape first.
- **Code and docs.** Pick an issue labelled `good first issue` or `help wanted`, or fix
  something that bothers you. For anything larger than a bug fix, open an issue first.

## Project priorities

When two goals conflict, the earlier one wins:

1. **Reliability.** Never lose a listening position, never crash on an odd file, always
   give the Kindle back in a working state.
2. **Efficiency.** Least possible battery, CPU and RAM on weak Kindle hardware.
3. **Minimal design** that suits e-ink.

## Development setup

macOS or Linux, with [Zig](https://ziglang.org) 0.17+, `clang`, `make` and FFmpeg on the
host (FFmpeg only generates test files).

```bash
./scripts/fetch-deps.sh          # pinned third-party sources and fonts → third_party/
./scripts/build-ffmpeg.sh host   # static FFmpeg for the tests
./scripts/build-ffmpeg.sh kindle # static FFmpeg for ARMv7 (only needed for `make package`)
make test                        # unit, integration and launcher tests
make sim                         # renders every screen to tests/golden/ and docs/screens/
make package                     # Kindle zip in dist/
```

You don't need a Kindle to work on most of Aloud: the simulator draws the real UI to PNG
files, and the test suite covers decoding, positions, the player and the launcher. If you
do have one, [DEVICE-TESTING.md](docs/DEVICE-TESTING.md) is the on-device checklist.

## Where things are

- `src/` — the app, one module per file (`store`, `decoder`, `mp4lite`, `library`,
  `player`, `audio_out_*`, `power`, `canvas`, `text`, `icons`, `ui`, `platform_*`, `main`).
- `helper/aloud_sink.c` — the small Bluetooth audio bridge to the firmware's GStreamer.
- `package/` — the launcher scripts and KUAL files that go into the zip.
- `extras/koreader/` — the KOReader plugin.
- `tests/` — fixtures (generated, never committed), C checks and the launcher test.
- `docs/ARCHITECTURE.md` explains each design decision and what was rejected.
  `docs/RESEARCH.md` collects Kindle platform facts with sources.

## Code style

- C11 with `-Wall -Wextra -Werror`. `snake_case`, with module prefixes (`ui_`, `lib_`,
  `store_`, `audio_`).
- Check every syscall and FFmpeg return value. Never `assert` on user data: show a message
  and carry on.
- No busy polling, no timers while paused, and no allocation in the audio path.
- Comments explain *why*: hardware quirks, firmware behaviour, and links to sources.
- New dependencies must be GPL-3-compatible and pinned in `scripts/fetch-deps.sh`
  (commit hash or SHA-256). Fonts must be OFL. Never commit `third_party/`, `build/` or
  `dist/`.

## Pull requests

1. Fork, branch from `main`, and keep the change focused.
2. Add or update tests for what you changed. `make test` must pass.
3. If you changed the UI, run `make sim` and attach before/after screenshots. If the README
   images are affected, run `make showcase`.
4. Write commit messages in the existing style: `fix(player): pause takes effect immediately`.
5. Say in the PR whether you tested on a Kindle, and which model and firmware.

By contributing you agree that your work is released under the
[GPL-3.0-or-later](LICENSE) licence, like the rest of Aloud.

## Code of conduct

Be kind. This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md).
