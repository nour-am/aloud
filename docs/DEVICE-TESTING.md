# Aloud — On-Device Testing Checklist

This checklist covers everything that only real Kindle hardware can verify (audio sink per
firmware generation, e-ink refresh behaviour, screensaver/suspend power management, crash
recovery, and battery drain).

---

## Phase 1: Diagnostics & Raw Audio / Power Probe (10 minutes)

Before testing full audiobooks, run the two standalone diagnostic scriptlets included in the
package (`documents/` or `extensions/aloud/bin/`):

1. **Device fact-finding (`tools/kindle-diagnose.sh`)**
   - Copy `tools/kindle-diagnose.sh` to `/mnt/us/documents/` on the Kindle over USB.
   - Eject USB and tap **Aloud diagnostics** in your Kindle Library.
   - Reconnect USB and inspect `/mnt/us/aloud-diagnose.txt` (serials and MACs are masked).
   - Verify:
     - Which GStreamer library exists (`/usr/lib/libgstreamer-1.0.so.0` on FW ≥ 5.19 vs
       `libgstreamer-0.10.so.0` on FW < 5.19).
     - Output of `gst-inspect mixersink` and `gst-inspect fdsrc`.
     - Output of `/proc/asound/cards` (empty on Bluetooth Kindles, populated on Kindle Touch).
     - Output of `lipc-probe -v com.lab126.powerd` and `com.lab126.btfd`.

2. **10-second 440 Hz tone + screensaver + `deferSuspend` test**
   - Connect Bluetooth headphones in **Settings › Bluetooth** (or plug headphones into a
     Kindle Touch).
   - From KUAL (**Aloud › Test Audio & Sleep**) or by copying `tools/kindle-audio-test.sh`
     to `documents/` and tapping **Aloud Audio Test**:
     - [ ] **Test 1A (Active playback):** Confirm a clean 440 Hz sine tone plays for 15 seconds
       without crackles or underruns.
     - [ ] **Test 1B (Screensaver via power button):** Start the test again, then press the
       Kindle power button after 2 seconds so the screensaver appears. Confirm whether the
       440 Hz tone continues playing for the remaining 13 seconds.
     - [ ] **Test 1C (`deferSuspend` on `readyToSuspend`):** While playing an audiobook in
       Aloud, press the power button and leave the Kindle in screensaver for > 90 seconds
       (past the ~60 s `Screen Saver → Ready to suspend` transition). Confirm audio continues
       playing uninterrupted.
     - *Fallback if Broadcom Bluetooth detaches on screensaver (RESEARCH.md §6.6 Unknown #7):*
       launch with `ALOUD_PREVENT_SCREENSAVER=1` in `extensions/aloud/aloud.sh`.

---

## Phase 2: Functional & UI Verification per Device Class

| Check | Expected Behaviour | Verified |
|---|---|---|
| **Launch from Library** | Tapping **Aloud** scriptlet in the Kindle Library opens Aloud cleanly with no statusbar bleed-through orpillow popups. | [ ] |
| **Status bar hidden** | No clock/battery bar is drawn over the Library or Chapters headers, even after the clock ticks over a minute; it reappears after Exit. If it still shows, put e.g. `6` in `extensions/aloud/state/top_inset_mm.txt` to lay Aloud out below it. | [ ] |
| **Launch from KUAL** | Launching from KUAL / KUALA / KUAL Next opens Aloud and prevents second-instance launch. | [ ] |
| **Touch & Rotation** | Taps hit exact buttons in Upright, Upside-Down, and Landscape orientations (including KT4 lift quirk and Oasis page-turn buttons). | [ ] |
| **Waveform quality** | Play/Pause and minute ticks update with fast non-flashing `DU`; sheets and page flips use `GL16`; cover loads flash cleanly with `GC16`. | [ ] |
| **M4B playback** | Single-file `.m4b` displays embedded chapters, embedded cover art (dithered), and accurate chapter scrubbing. | [ ] |
| **MP3 folder playback** | Multi-file `.mp3` folder orders chapters naturally (`1, 2, …, 10`), transitions seamlessly across file boundaries, and displays `cover.jpg`. | [ ] |
| **Speed control** | `0.75×` through `3.0×` changes speech tempo without pitch distortion (`sonic`) and scales "hours left" on the Player screen. | [ ] |
| **Pause & Resume** | Pausing stops audio immediately; resuming rewinds 2 seconds and continues smoothly. | [ ] |
| **Sleep timer & Fade** | Setting a sleep timer (or `End of chapter`) fades volume smoothly over the final 10 seconds, pauses, saves position, and allows normal device suspend. | [ ] |

---

## Phase 3: Crash Safety & Persistence Stress Tests

- [ ] **SIGKILL / Crash restore test:** While Aloud is running in the foreground, SSH/kterm
  in and run `kill -9 $(pidof aloud)`. Confirm `aloud.sh`'s `trap` restores `volumd`,
  `awesome`, `statusbar`, the framebuffer dump, and `pillow`, returning the Kindle to the
  stock Home screen in a working state.
- [ ] **USB mass-storage lock:** While Aloud is open, plug the Kindle into a computer over
  USB. Confirm `volumd` is held (`SIGSTOP`) so the `/mnt/us` filesystem is not unmounted out
  from under open audiobook files; confirm USB mounting works normally again after exiting
  Aloud.
- [ ] **Corrupt state recovery:** Truncate `/mnt/us/extensions/aloud/state/positions.tsv` by
  hand and relaunch Aloud; confirm it automatically restores the previous listening position
  from `positions.tsv.bak`.

---

## Phase 4: Battery & Resource Benchmark

- [ ] **Idle paused CPU usage:** Pause playback in Aloud, wait 2 minutes (so `aloud-sink`
  closes), and check `top` over SSH/kterm. Confirm `aloud` consumes **0.0% CPU** (blocked in
  `poll()` with timeout `-1`).
- [ ] **Playing CPU & RAM usage:** While playing a 64 kbps AAC `.m4b` at `1.25×` speed with
  the screen idle, confirm `aloud` uses `< 5%` CPU and `< 15 MB` RSS.
- [ ] **1-hour battery drain test:** Charge to 100%, play an audiobook over Bluetooth for
  60 minutes with the screensaver active, and record battery % drop vs. 60 minutes of the
  stock Audible player.
