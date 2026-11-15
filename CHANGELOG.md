# Changelog

All notable changes to Aloud. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and versions follow [Semantic Versioning](https://semver.org).

## [Unreleased]

## [0.1.0] - 2026-11-15

First public release.

### Added

- Standalone audiobook player for jailbroken Kindles on FW 5.x, launched from the Kindle
  library or KUAL.
- Single-file books with embedded chapters (`.m4b`, `.m4a`, `.mp3`, `.ogg`, `.opus`,
  `.flac`) and multi-file chapter folders with natural sorting.
- Bluetooth output through the firmware's `mixersink`; headphone jack and speaker on the
  Kindle Touch.
- Play in background while reading, with KUAL controls and a KOReader plugin.
- Pitch-preserving speed (0.75×–3×), sleep timer with fade-out and end-of-chapter mode.
- Atomic position saves with backup recovery; 2-second rewind on resume.
- Low-memory M4B reader (4.7 MB peak on an 8.6-hour book).
- Latin, Greek, Cyrillic and Arabic text, including right-to-left shaping.
- On-device diagnostics, audio test and performance measurement from KUAL.

[Unreleased]: https://github.com/NoureldinAmer/aloud/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/NoureldinAmer/aloud/releases/tag/v0.1.0
