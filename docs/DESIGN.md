# Aloud — Product & UI design spec

Status: **agreed direction, not yet implemented.** Mock-ups will be produced by the host
simulator (`make sim`) and committed under `docs/screens/` once the UI exists.

## Product goal

The go-to audiobook player for jailbroken Kindles. Standalone (no KOReader dependency),
installed like any KUAL extension, playing a user's own DRM-free audiobooks
(M4B/M4A/MP3/OGG/Opus/FLAC) to the Kindle's audio output (Bluetooth on modern Kindles,
headphone jack/speaker on the Kindle Touch).

Ranked priorities:

1. **Reliability** — never lose the listening position, never crash on odd files, always
   restore the Kindle UI on exit (even after a crash).
2. **Battery & resource efficiency** — no busy loops, few wakeups, minimal e-ink refreshes,
   small memory footprint.
3. **Pristine, minimal design** — feels native to e-ink, nothing decorative.

## Design principles

1. **E-ink first.** No animation, no scrolling, no spinners. Lists paginate. Every screen
   refresh is deliberate and as small as possible.
2. **Physical sizes, not pixels.** All layout uses millimetres converted with the panel DPI
   reported by FBInk (`FBInkState.screen_dpi`), so a 6" 167 ppi Kindle and a 300 ppi Scribe
   look the same. Minimum touch target: **9 mm** square. Primary play button: **18 mm**.
3. **Four tones only.** Black `0x00`, ink-gray `0x55` (secondary text), rule-gray `0xAA`
   (hairlines, inactive track), white `0xFF`. No large mid-gray fills (they ghost and look
   muddy). Covers are the only full-tone content and are dithered to the 16-level palette.
4. **Two typefaces.** A serif for book titles (reading-culture feel), a sans for UI and
   numbers. Both bundled (OFL) so every device renders identically. Use tabular figures for
   times so digits don't jitter between updates.
5. **Glanceable.** The player screen answers, without any tap: what book, which chapter,
   how far in, how long left, is it playing.
6. **Nothing hidden behind gestures.** Every action has a visible tap target. Swipes and
   hardware page-turn buttons are accelerators only.

## Screens

### Library
- Header: "Library" (sans, bold) left; **✕ Exit** right.
- Rows (one per book), most-recently-played first, then by title:
  square cover thumbnail (≈ 18 mm) · title (serif, ≤ 2 lines, ellipsis) · author (sans,
  ink-gray) · thin progress bar + "42% · 3 h 10 m left" (or "New" / "Finished").
- Footer: "‹ 1 / 3 ›" pagination. Swipe left/right and page-turn buttons also paginate.
- Empty state: "No audiobooks yet" + one line on where to copy files over USB.
- Tap a row → Player for that book (resumes at the saved position, does not auto-play
  unless it was playing when the app last exited).

### Player (Now Playing)
Top to bottom:
- Top bar: **‹ Library** left, **Play in background** (outlined button) centre,
  **Chapters** right. The centre label shortens to "Background" or is omitted on panels
  too narrow for it.
- Cover art, centred, ≈ 55 % of screen width (placeholder: title set in the serif on a
  hairline frame when the book has no art).
- Title (serif) and author (sans, ink-gray), centred.
- Chapter title, and "Chapter 3 of 24".
- Chapter scrubber: hairline track, black fill to the current position, tap anywhere on
  it to seek within the chapter. Below: elapsed (left) and remaining (right), tabular.
- Book line, small: "Book 42% · 6 h 2 m left".
- Transport row (equal 9 mm+ targets): ⏮ previous chapter · ↺ 30 · **▶/⏸** (18 mm
  black disc) · ↻ 30 · ⏭ next chapter. Skip intervals configurable (default 30 s back,
  30 s forward). "Previous chapter" within the first 3 s of a chapter goes to the chapter
  before; otherwise restarts the current one (standard player behaviour).
- Bottom row: **Speed 1.0×** · **Sleep Off** · volume − / + (only shown when volume is
  controllable on this device — see RESEARCH.md).

### Background playback (ARCHITECTURE.md D13)
- **Play in background** hands the screen back to the Kindle exactly as it was (book,
  home, KUAL) and keeps playing. Nothing of Aloud is drawn while in the background.
- Opening Aloud again (KUAL "Open Aloud" or the Aloud scriptlet) returns to the player,
  live, instead of starting over.
- KUAL "Play / Pause", "Back 30 s", "Forward 30 s" control it without opening it.
- In KOReader, the bundled plugin adds Tools → Aloud audiobook (play / pause, ±30 s,
  previous / next chapter) and the same actions for the Gesture manager; each tap shows
  a short "Aloud: playing · Chapter 3" notification.
- It stops by itself, saving the position, when a cable is plugged in or after 10 min
  paused.

### Chapters
- Paginated list: chapter title, and right-aligned the time in the book where it starts
  (`mm:ss`, or `h:mm:ss` on every row once the book is over an hour). Current chapter in bold with a
  black left bar. Tap → seek to chapter start and return to Player.

### Sheets (Speed, Sleep)
- Bottom sheet with a hairline top border; options as a grid of 9 mm buttons; the active
  one is inverted (white on black).
- Speed: 0.75 · 1.0 · 1.1 · 1.25 · 1.5 · 1.75 · 2.0 · 2.5 · 3.0 (pitch-preserving).
- Sleep: Off · 5 · 15 · 30 · 45 · 60 min · End of chapter. When the timer fires: fade the
  last 10 s of audio, pause, save. The bottom-row label shows remaining minutes (updated
  only on the regular minute tick).

### Messages
- Single-line toast area above the transport row, cleared on the next interaction.
  Examples: "No audio output — pair headphones in Settings › Bluetooth", "Can't play
  ‘file.wma’ (unsupported format) — skipped".

## Refresh policy (battery + ghosting)

| Change | Region | Waveform |
|---|---|---|
| Play/pause icon toggle, time labels, scrubber | that widget only | DU (fast, B/W) or GL16 for gray text |
| Screen-to-screen navigation | full screen | GL16/REAGL partial; flashing GC16 every 6th full repaint or when a cover is drawn |
| Page change in a list | list area | GL16 partial |
| Periodic progress tick while playing | time labels + scrubber | DU, once per second while the Player is on screen |
| Screen is in screensaver/off | nothing | no drawing at all |

The tick is skipped entirely when nothing visible changed (the displayed second and the
sleep-timer minute are the same). It was once per minute until device feedback showed a
stalled-looking clock reads as a frozen player; it never runs while paused, in the
screensaver, or in the background.

## Content layout on the Kindle

```
/mnt/us/audiobooks/                 ← user copies books here over USB
    Author - Title/                 ← a folder of MP3s = one book, files are chapters
        01 Chapter One.mp3
        cover.jpg                   ← optional; embedded art preferred if present
    Another Book.m4b                ← a single file = one book (embedded chapters used)
/mnt/us/extensions/aloud/           ← the app (KUAL extension)
    state/                          ← positions, library cache, thumbnails (kept on update)
```

Book discovery rules:
- A top-level file with a supported extension is one book.
- A top-level folder containing supported files is one book; files are ordered by
  natural sort of the file name (so "2" < "10"), falling back to track-number tags only
  when names don't differ in a number. Nested sub-folders (e.g. "Disc 1", "Disc 2") are
  flattened in natural order.
- Title: album tag → folder/file name. Author: album_artist → artist → "Unknown author".
- Chapters: embedded chapters (M4B `chpl`/QuickTime chapter track, MP3 ID3 `CHAP`) when
  present; otherwise one chapter per file.
