# Aloud — Kindle platform research

Facts gathered on 2026-11-03 from code, configs and forum posts. Every claim carries its
source. Items marked **(inference)** are reasoned, not stated by a source. Items in the
"Unknown" lists still need confirmation, ideally on real hardware.

## 1. Jailbreak ecosystem (as of Nov 2026)

Source for the table: the wiki's wizard data at https://kindlemodding.org/jailbreaks.json
(drives https://kindlemodding.org/jailbreak-wizard.html).

| Jailbreak | Firmware range | Notes |
|---|---|---|
| WinterBreak (Mesquito, 1 Jan 2025) | 5.6.1.1–5.18.0.2 | Device must be registrable; not ≥ 5.18.1 (https://kindlemodding.org/jailbreaking/WinterBreak/) |
| WinterBreak2 | 5.6.1.1–5.16.3, 5.16.5 | |
| SpiderCat | 5.16.3–5.19.5 | |
| Véra | 5.17.1–5.20.1.0.2 | PW5, KT5, KT6, PW6, CS, KS, KS2 |
| Nosebleed | 5.16.4–5.18.6 | KT5, PW5, KOA3 (PW6/KT6 ≤ 5.17.1.0.4) |
| Sanctuary | 5.16.4–5.18.3 | excludes 5.16.5 |
| SpringBreak | 5.18.1–5.19.2.0.1 | KT5, PW5 |
| AdBreak | 5.18.1–5.18.5.0.1 | ad-supported devices only |
| Shadow ("MTP jailbreak") | 5.19.4–5.19.6 | KS, KS2, CS, PW6, KT6 |

- LanguageBreak (≤ 5.16.2.1.1) is now "Legacy" (https://github.com/KindleModding/LanguageBreak).
- No jailbreak for Scribe 3, Scribe Colorsoft, Kindle 2026, Paperwhite 2026
  (https://kindlemodding.org/models.json).
- **Universal Hotfix** (NiLuJe's hotfix for armel+armhf + sh_integration + KPM) must be
  re-run after every OTA; WinterBreak/SpringBreak/Sanctuary include it
  (https://github.com/KindleModding/Hotfix,
  https://kindlemodding.org/jailbreaking/Legacy/post-jailbreak/setting-up-a-hotfix/).

### Hard-float switch at FW 5.16.3
- Amazon "switched … from softfloats to hardfloats"; nothing armel-only runs on it
  (https://github.com/koreader/koreader/discussions/11298).
- KOReader detects it via `/lib/ld-linux-armhf.so.3` and picks `kindlehf`, else
  `kindlepw2`/`kindle` (https://github.com/koreader/koreader/blob/master/frontend/device/kindle/device.lua).
- KOReader ships four Kindle zips per release: kindlehf, kindlepw2, kindle, kindle-legacy
  (https://github.com/koreader/koreader/releases).
- **Aloud's answer:** one fully static musl binary works on both (ARCHITECTURE.md D2).
  The launcher script may still need the armel/armhf distinction for nothing else.

### Toolchains (for reference; Aloud uses Zig instead)
koxtoolchain (https://github.com/koreader/koxtoolchain, sample configs at
https://github.com/benoit-pierre/crosstool-ng/tree/34844bc8e985ad1ba26b072a5b58264967072e19/samples):

| TC | Devices | glibc / kernel headers | Float / CPU |
|---|---|---|---|
| kindle | K2, DX, DXg, K3 | 2.9 / 2.6.27 | softfp, arm1136jf-s |
| kindle5 | K4, Touch, PW1 | 2.12.2 / 2.6.32 | softfp, cortex-a8, NEON |
| kindlepw2 | PW2+ on FW < 5.16.3 | 2.12.2 / 3.0 | softfp, armv7-a, cortex-a9 tune, NEON |
| kindlehf | any Kindle on FW ≥ 5.16.3 | 2.20 / 4.1 | hard, armv7-a, cortex-a7 tune, NEON |

These are glibc *floors*; actual on-device versions are unknown (see Unknown).

## 2. Distribution & launching entry points

### Scriptlets — primary entry point
- A `.sh` in `/mnt/us/documents` is indexed by SH_Integration as a library "book"; tapping
  it runs it (https://kindlemodding.org/kindle-dev/scriptlets.html). Header:
  ```sh
  #!/bin/sh
  # Name: Aloud
  # Author: …
  # Icon: <path or base64 image>
  # DontUseFBInk
  ```
  `# DontUseFBInk` is required for a full-screen app (otherwise stdout/stderr are piped to
  FBInk on screen). `# UseHooks` enables `on_install`/`on_remove`/`on_run`.

### KUAL — secondary entry point
- Classic Java KUAL is **deprecated from FW 5.19.4**: "5.19.3+ removed support for the old
  Java API" (https://github.com/koreader/koreader/wiki/Installation-on-Kindle-devices,
  https://kindlemodding.org/jailbreaking/faq/migration.html,
  https://www.mobileread.com/forums/showthread.php?p=4605281).
- Replacements read the same `config.xml`/`menu.json`: KUALA (C++/GTK, 5.16.3+) and
  **KUAL Next** (C + FBInk, kindlehf — https://github.com/thiagokokada/kual-next; a very
  close architectural reference for Aloud).
- Extension layout `/mnt/us/extensions/<name>/` — real example (KOReader,
  https://github.com/koreader/koreader/tree/master/platform/kindle/extensions/koreader):
  ```xml
  <extension><information><name>KOReader</name><version>1.1</version><author>KOReader Dev Team</author><id>KOReader</id></information>
  <menus><menu type="json" dynamic="true">menu.json</menu></menus></extension>
  ```
  ```json
  {"items":[{"name":"KOReader","priority":0,"items":[
   {"name":"Start KOReader (no framework)","priority":4,"action":"/mnt/us/koreader/koreader.sh",
    "params":"--kual --framework_stop","status":false,"internal":"status Kill the framework and start KOReader"}]}]}
  ```
  Other keys: `if`, `exitmenu`, `checked`, `refresh`, `date`. Log: `/mnt/us/extensions/KUAL.log`
  (https://wiki.mobileread.com/wiki/KUAL_What's_New).

### KPM — future entry point
- `;kpm install <id>` from the home search bar. Package = `manifest.json`
  (`"manifest_version": 2, "id", "name", "author", "version": [1,0,0], "dependencies",
  "supported_platforms"`) + optional `install.sh`/`launch.sh`/`uninstall.sh`; the package
  installs its own scriptlet calling `/var/local/kmc/bin/kpm launch <id>`
  (https://github.com/KindleModding/example_kpm_package,
  https://kindlemodding.org/jailbreaking/whats-next/getting-koreader/).

### MRPI `.bin` — not needed
- Only needed to write outside `/mnt/us`. `kindletool create ota2 -d kindle5 -C pkgdir
  Update_x_install.bin` (https://github.com/NiLuJe/KindleTool;
  https://www.mobileread.com/forums/showthread.php?t=251143).

### How others ship
- KOReader: zip with `koreader/` + `extensions/koreader/`, "unzip … into the Kindle's USB
  root" (https://github.com/koreader/koreader/blob/master/make/kindle.mk).
- kterm: separate armel/armhf zips into `extensions/kterm` (https://github.com/bfabiszewski/kterm).
- KUAL Next: `documents/KUAL Next.sh` + `kual-next/bin/kual-next`.

**Decision for Aloud:** one zip, unzipped onto the USB root:
```
documents/Aloud.sh                 scriptlet (# DontUseFBInk) → exec /mnt/us/extensions/aloud/aloud.sh
extensions/aloud/config.xml        KUAL/KUALA/KUAL Next entry
extensions/aloud/menu.json
extensions/aloud/aloud.sh          launcher (prepare UI, run, always restore)
extensions/aloud/bin/aloud         static ARMv7 binary
extensions/aloud/fonts/…           bundled OFL fonts
audiobooks/README.txt              where to put books
```
A KPM package can wrap the same tree later.

## 3. Taking over the screen — what KOReader does

Source: https://github.com/koreader/koreader/blob/master/platform/kindle/koreader.sh

Launch (framework kept running — the default and what Aloud should do, since we need the
stock power/Bluetooth stack alive):
1. Re-exec from `/var/tmp` (because `/mnt/us` is a vfat+fuse overlay).
2. If a passcode is set, remove `/var/local/system/userpasswdenabled` (restore on exit).
3. `cat /dev/fb0 >/var/tmp/koreader-fb.dump` (restore later).
4. FW ≥ 5.6.5: `lipc-set-prop com.lab126.pillow disableEnablePillow disable`.
   FW ≥ 5.7.2: `killall -STOP awesome`.
   Older: `lipc-set-prop com.lab126.pillow interrogatePillow '{"pillowId": "default_status_bar", "function": "nativeBridge.hideMe();"}'`.
5. If `/etc/upstart/statusbar.conf` exists: `stop statusbar` (KPP chrome bar is a separate
   job; verified on PW6 @ 5.17.1.0.4 & 5.19.4.0.1, PW4 @ 5.18.1).
6. Optionally stop services to free RAM: `stored webreader kfxreader kfxview todo tmd rcm
   archive scanner otav3 otaupd`.
7. `killall -STOP volumd` to block USB mass storage while running.

Exit: `killall -CONT volumd`, restart stopped jobs, `killall -CONT awesome`, restore the fb
dump, then
```sh
lipc-set-prop com.lab126.pillow disableEnablePillow enable
lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home
```
(No-framework mode instead: `stop lab126_gui` on launch, `start lab126_gui` on exit.)

MTK: KOReader calls `_MTK_ToggleFastMode(true)` so the driver doesn't silently promote
refreshes to REAGL, off on exit; on Scribe it briefly `killall -CONT awesome` so lipc
calls work.

## 4. Power events

- KOReader does **not** block the screensaver; it listens with
  `lipc-wait-event -m -s 0 com.lab126.powerd goingToScreenSaver,outOfScreenSaver,exitingScreenSaver,charging,notCharging,wakeupFromSuspend,readyToSuspend`
  and draws its own screen
  (https://github.com/koreader/koreader-base/blob/master/input/input-kindle.h).
  Event source codes: 1 = button wake, 2 = button suspend, 4/6 = cover (hall sensor).
- To block the screensaver: `lipc-set-prop com.lab126.powerd preventScreenSaver 1` (int, rw)
  (https://kindlemodding.org/kindle-apps-and-services/com.lab126.powerd.html,
  https://www.mobileread.com/forums/showthread.php?p=1976504).
- Power button: use these lipc events rather than evdev.
- Audio-specific power behaviour (keeping the CPU out of suspend while playing):
  see §6.6.

## 5. Display & input

### FBInk (https://github.com/NiLuJe/FBInk)
- GPL-3.0-or-later. Last tag v1.25.0 (Dec 2022) but master is active (Aug 2026) and is
  required for MTK devices → we pin a master commit (scripts/fetch-deps.sh).
- Lifecycle: `fbink_open` → `fbink_init(fd,&cfg)` → `fbink_get_state` → draw →
  `fbink_close`. Zeroed `FBInkConfig` is valid.
- `FBInkState`: `view_width/height`, `screen_dpi`, `bpp`, `inverted_grayscale`,
  `device_name/id`, `is_mtk`, `has_color_panel`, `current_rota`, `rotation_map[4]`.
- `fbink_print_raw_data(fd,data,w,h,len,x,y,&cfg)`: Y/YA/RGB/RGBA; prefer Y (+`ignore_alpha`)
  on Kindle. `fbink_refresh(fd,top,left,w,h,&cfg)`; `cfg.is_flashing` for full updates;
  `no_refresh` to batch; `fbink_wait_for_complete(fd, LAST_MARKER)` to fence.
- Waveforms: DU, GC16, GL16, REAGL(D), A2, GC16_FAST, GL16_FAST, DU4, … Header note: on
  all Kindle mxcfb and most MTK devices, **REAGL/REAGLD expect to always be flashing**.
  KOReader's Kindle mapping: fast = DU, full = GC16, partial = REAGL (GLR16 on MTK),
  night = GL16_INV/GLKW16 (https://github.com/koreader/koreader-base/blob/master/ffi/framebuffer_mxcfb.lua).
- HW dithering: KOReader says it does **not** work on PW5/KT (Zelda/Rex); only Scribe and
  Colorsoft. → Aloud dithers covers in software.
- **Rotation: FBInk does no software rotation, and "X will not like you rotating the fb
  from under its feet."** → Aloud must not change fb rotation; it lays out for whatever
  orientation the fb is in (gyro devices: `lipc-get-prop com.lab126.winmgr accelerometer`
  returns U/R/D/L).
- Build flags: `KINDLE=1`, `MINIMAL=1` + `DRAW`/`BITMAP`/`FONTS`/`IMAGE`/`OPENTYPE`/`INPUT`.
  KUAL Next: `make staticlib KINDLE=1 MINIMAL=1 BITMAP=1 DRAW=1 INPUT=1 OPENTYPE=1`.
  Aloud compiles the sources directly instead (Makefile AR/RANLIB defaults to gcc-ar and the
  staticlib target drags in i2c-tools). Verified: `fbink.c cutef8/*.c fbink_input_scan.c`
  compile with `tools/arm-cc -DFBINK_FOR_KINDLE -DFBINK_MINIMAL -DFBINK_WITH_INPUT`.

### Touch
- Find devices with `fbink_input_scan(INPUT_TOUCHSCREEN|INPUT_PAGINATION_BUTTONS|INPUT_HOME_BUTTON, INPUT_POWER_BUTTON, NO_RECAP, &n)`; free the result.
  KOReader's fallback paths: Touch event3; PW1/KT3 event0; PW2/KT2/KV/PW3 event1; Oasis 1
  `/dev/input/by-path/platform-imx-i2c.1-event`; Oasis 2/3
  `/dev/input/by-path/platform-30a30000.i2c-event`; PW4/KT4 event2 (event3 on LTE);
  Scribe `/dev/input/touch`; PW5/KT5/PW6/KT6 must scan.
- Protocol B (`ABS_MT_SLOT`, `ABS_MT_TRACKING_ID`, `ABS_MT_POSITION_X/Y`)
  (https://github.com/koreader/koreader/blob/master/frontend/device/input.lua).
  **KT4 never sends `TRACKING_ID -1` on lift → also treat `BTN_TOUCH 0` as lift.**
  Kindle Touch reports 0–4095 → must scale to 600×800.
- `ioctl(fd, EVIOCGRAB, 1)` on every opened device
  (https://github.com/koreader/koreader-base/blob/master/input/input.c).
- **FBInk sets no touch swap/mirror quirks for any Kindle** (Kobo/Cervantes only —
  https://github.com/NiLuJe/FBInk/blob/master/fbink_device_id.c). Mapping that works
  (KUAL Next, https://github.com/thiagokokada/kual-next/blob/HEAD/src/ui_fbink.c): read axis
  ranges with `EVIOCGABS`, normalise, swap axes if the raw aspect ratio differs from the
  view's, then apply `rotation_map`.

### Keys
- Oasis 1/2/3: 104 = page forward, 109 = page back ("the reverse than on non-Oasis").
  Voyage: 104 = back, 109 = forward.
- PW6/Colorsoft frame double-tap: keycode 65 from a device classified
  `INPUT_KINDLE_FRAME_TAP`.
- Hotplug: `libue.h` (MIT) in koreader-base listens to netlink uevents.

### Screens (read the real values from FBInkState at runtime)

| Model | Resolution | ppi | SoC |
|---|---|---|---|
| PW4 (2018) | 1072×1448 | 300 | i.MX6SLL |
| PW5 (2021) | 1236×1648 | 300 | MT8110 |
| PW6 (2024) | 1272×1696 | 300 | MTK |
| KT4 (2019) | 600×800 | 167 | i.MX |
| KT5 (2022) / KT6 (2024) | 1072×1448 | 300 | MT8113 |
| Oasis 2/3 | 1264×1680 | 300 | i.MX7D |
| Scribe 2022/2024 | 1860×2480 | 300 | MTK |
| Colorsoft | 1264×1680 | 300 B&W / 150 colour | MTK |

### Fonts on device
- `/usr/java/lib/fonts` (a mounted `fonts.sqsh`); KOReader also searches `/mnt/us/fonts`,
  `/var/local/font/mnt`, `/mnt/us/linkfonts/fonts`. It may vanish with the Java removal on
  5.19.3+ (unconfirmed) → **Aloud bundles its own OFL fonts.**
- Never hand FBInk/stb_truetype a non-font or obfuscated file (segfault risk) → validate
  with `stbtt_GetFontOffsetForIndex` / table checks before use.

### Unknown (display/packaging)
- Actual glibc version per firmware (only toolchain floors known).
- Whether MRPI still works on 5.19.4+ without KUAL; KUALA's source.
- Whether `/usr/java/lib/fonts` exists after 5.19.3+.
- Whether `preventScreenSaver` still works on 5.16+ and MTK.
- Whether the power button appears as an evdev device.
- Real resolutions for PW3, Scribe 3, Scribe Colorsoft; fb stride/padding on PW6/Colorsoft.

## 6. Audio & Bluetooth

Labels: **[code]** from source that runs on devices, **[forum]** hands-on report,
**(inference)** reasoned. Several forum facts came through summaries — re-read the cited
page before relying on an exact quote.

### 6.1 Which Kindles can make sound
- **Kindle Touch (K5, 2011, i.MX508):** the only FW 5.x Kindle with speakers + 3.5 mm jack.
  Real ALSA card (`/dev/snd/pcmC0D0p`, `controlC0`, kernel 2.6.31-rt11-lab126); mplayer
  works [forum] (https://wiki.mobileread.com/wiki/Kindle_Touch,
  https://www.mobileread.com/forums/showthread.php?p=2101882). Built-in `aplayer`; "100% is
  actually 150%" software boost (https://www.mobileread.com/forums/showthread.php?p=4208086).
- **No audio:** PW1, PW2, PW3, Voyage, KT2 (PW3/Voyage/Oasis 1 only via the USB "Kindle
  Audio Adapter"; a PW3 with a USB OTG sound card has `aplay`/`amixer` and `sox | aplay`
  worked — https://www.mobileread.com/forums/showthread.php?t=329588&page=2).
- **Bluetooth, Broadcom BCM4343 on UART `/dev/ttymxc2`:** KT3, Oasis 1, Oasis 2/3 (i.MX7D),
  PW4 and KT4 (i.MX6SLL, kernel 4.1.15) [code]
  (https://github.com/zampierilucas/kindle-hid-passthrough/blob/main/kindle_hid_passthrough/kindle_detect.py).
- **Bluetooth, MediaTek CONSYS `/dev/stpbt`:** PW5 (MT8110 "Bellatrix", kernel 4.9.77),
  KT5, Scribe, KT6, PW6, Scribe 2, Colorsoft, Scribe 3, Scribe Colorsoft [code] (same file).

### 6.2 The stock stack — **no ALSA card on Bluetooth models**
- No BlueZ, PulseAudio or bluealsa. PW4 5.12.3: `/proc/asound/cards` → "--- no soundcards
  ---"; `aplay -l` finds nothing; mplayer ALSA/OSS fail [forum]
  (https://www.mobileread.com/forums/showthread.php?t=313397,
  https://www.mobileread.com/forums/showthread.php?p=3972176). PW5 stack is "provided by
  the chip vendor" (https://www.mobileread.com/forums/showthread.php?p=4305546).
- Broadcom daemons: `btd -f` → `bsa_server` (Broadcom BSA), `audiomgrd`, `playermgr`
  (Audible), `ivonattsd`, `btfd`; state in `/var/local/zbluetooth/` [forum/code]
  (https://www.mobileread.com/forums/showthread.php?t=328630&page=2,
  https://github.com/zampierilucas/kindle-hid-passthrough/blob/main/kindle_hid_passthrough/bt_brcm.py).
- MediaTek daemons: `acsbtfd`, `btmanagerd`, `wmt_cli`; CLI `ace_bt_cli` (`radiostate`,
  `enable`, `classic scan`, `pair <addr>`, `connectedlist`) [forum, PW5/PW6/Scribe @ 5.18]
  (https://www.mobileread.com/forums/showthread.php?p=4534501). Paired devices in
  `/var/local/zbluetooth/bt_config.conf` (Fluoride-like format — inference).
- **Stock audio path:** `playermgr` runs `audiblesrc ! mixersink` (GStreamer 0.10).
  `libgstmixersink.so` ("EINK-specific Mixer GStreamer sink", proprietary) wraps
  `libmixerAPI.so` (shared-memory `mstream` in `/dev/shm`, `/var/run/mixer`). `gst-inspect`
  on PW4 lists only coreelements, `mixersink`, `audiblesrc` — no wavparse/audioconvert.
  PW4 rootfs has `gst-launch(-0.10)`, `amixer`, `alsamixer`
  (https://www.mobileread.com/forums/showthread.php?t=328630&page=2 and page=3).
- `mixersink` confirmed on PW5 @ 5.14.2 and Oasis 3
  (https://www.mobileread.com/forums/showthread.php?t=336390); KinAMP: "Lab126's own sink,
  the only one on the device" [code] (https://github.com/kbarni/KinAMP/blob/HEAD/music_backend.cpp).
- **FW 5.19 updated GStreamer to 1.0** ("The GStreamer library was updated on firmware
  5.19" — https://github.com/kbarni/LARKPlayer/issues/10). KinAMP ships GStreamer-1.0
  builds and picks by which `libgstreamer-{0.10,1.0}.so.0` exists
  (https://github.com/kbarni/KinAMP/blob/HEAD/scripts/startkinamp.sh). Whether `mixersink`
  exists on 5.19+ is unconfirmed; a PW 12th-gen on 5.19.6 got "Internal data stream error".

### 6.3 Proven recipes for arbitrary audio
- PW4 (2020): `sox file -t raw - | gst-launch filesrc location=/dev/stdin !
  audio/x-raw-int,endianness=1234,signed=true,width=16,depth=16,rate=44100,channels=2 !
  queue ! mixersink` [forum] (https://www.mobileread.com/forums/showthread.php?t=328630&page=5).
  **22050 Hz mono produced silence** (page=3) — every confirmed recipe is 44.1 kHz stereo
  S16LE. Became the "Sox Media Player" extension; "Playback stops when Kindle goes to deep
  sleep" (https://www.mobileread.com/forums/showthread.php?t=336390).
- **KinAMP** (C++/GTK2 music player + KOReader plugin, 2025–26, works on 11th gen):
  miniaudio/FAAD decode → S16 stereo PCM → FIFO `/tmp/kinamp_audio_pipe` → in-process
  `gst_parse_launch` pipeline into `mixersink` [code]
  (https://github.com/kbarni/KinAMP/blob/HEAD/HACKING.md). Exact pipelines (from
  music_backend.cpp):
  - GStreamer 1.0: `filesrc location="<fifo>" ! audio/x-raw, format=S16LE, layout=interleaved, rate=%d, channels=2 ! queue ! mixersink`
  - GStreamer 0.10: `filesrc location="<fifo>" ! audio/x-raw-int, endianness=1234, signed=true, width=16, depth=16, rate=%d, channels=2 ! queue ! mixersink`
  FIFOs must live on `/tmp`, not on vfat `/mnt/us`.
- **LARK** (https://github.com/kbarni/LARKPlayer): an existing M4B/MP3 audiobook player by
  the KinAMP author on the same engine; FW < 5.19 only. **This is Aloud's closest
  competitor** — differentiate on all-firmware support, e-ink design and battery behaviour.
- Built-in `mplay` played WAV over BT on PW4
  (https://www.mobileread.com/forums/showthread.php?t=313397&page=2).
- KOReader has no audio of its own.

### 6.4 Bluetooth control via lipc (`com.lab126.btfd`)
- `BTenable` ("1:1" on, "0:1" off), `BTstate`, `Connect`/`Disconnect <MAC>`, `Bond`,
  `ListPaired`/`ListConnected` (hasharrays), `BTconnectedDevName`, `ensureBTconnection`
  (https://kindlemodding.org/kindle-apps-and-services/com.lab126.btfd.html,
  https://github.com/kbarni/KinAMP/blob/HEAD/kinamp.koplugin/kinamp_bt.lua).
- Stock pairing UI: `com.lab126.pillow customDialog` with `bt_wizard_dialog` [code]
  (https://github.com/kbarni/KinAMP/blob/HEAD/music_player.cpp). Settings › Bluetooth only
  appears once the device is registered (https://www.mobileread.com/forums/showthread.php?p=4305546).
- `Disconnect_Result` event on btfd; reconnect with `Connect <MAC>` (PW5 @ 5.18.5,
  https://github.com/imanubdesigner/kindle-bt-keepalive).
- **BT audio drops after 20 min idle** unless `ensureBTconnection` is 1; btfd only reads
  it when the radio comes up, so the stock sequence cycles `BTenable 0:1 → flag → 1:1`,
  which drops headphones for seconds [code]
  (https://github.com/kbarni/KinAMP/blob/HEAD/cli_player.cpp).
- Uncertain: KinAMP says 11th-gen devices "drive libace_bt instead" of btfd, yet
  kindle-bt-keepalive uses btfd on a PW5. `BTenable` second field ("1:1" vs "1:2") meaning
  unknown.
- `com.lab126.audiomgrd` (KT3): `isStarted`, `audioOutputConnected` [code]
  (https://github.com/yzgpk2002/kindle8-offline-home/blob/main/src/app/audio_lifecycle.py).

### 6.5 Volume
- No usable ALSA mixer on BT models. KinAMP scales PCM in software. Users set the device
  level in Settings › Accessibility › VoiceView › Volume or in the Audible player
  (https://www.mobileread.com/forums/showthread.php?t=336390). PW4 `/etc/audio/volume`
  defines "speaker"/"headphone" sinks. No lipc volume property or AVRCP absolute volume
  handling is known.

### 6.6 Power while playing
- powerd properties: `preventScreenSaver` (rw), `deferSuspend`, `state`, `status`,
  `rtcWakeup`, `wakeUp`, `powerButton`, `touchScreenSaverTimeout`, `abortSuspend`
  (https://kindlemodding.org/kindle-apps-and-services/com.lab126.powerd.html).
- States (Kindle Touch): Active ~10 min → Screen Saver ~1 min → Ready to suspend ~5 s (KOReader
  comment: 10 s) → suspend. `deferSuspend` only accepted during Ready to suspend
  (`lipcErrNoSuchProperty` otherwise); `deferSuspend 3000000` there extended it ~3,000,000 s.
  `powerd_test -s` prints state + time left
  (https://www.mobileread.com/forums/showthread.php?p=2610501,
  https://github.com/koreader/koreader/blob/master/frontend/device/kindle/powerd.lua).
- Events: `goingToScreenSaver <src>`, `outOfScreenSaver <src>`, `exitingScreenSaver`,
  `t1TimerReset`, `readyToSuspend <delay>`, `suspending`, `wakeupFromSuspend <secs>`
  (https://www.mobileread.com/forums/showthread.php?p=4507607). Sources: 1/6 button/cover
  wake, 2/4 button/cover suspend (KOReader device.lua).
- **CPU keeps running in screensaver; only the real suspend stops audio** (old Kindles:
  stops "about 60 seconds after … screensaver" —
  https://www.mobileread.com/forums/showthread.php?p=1718431). MediaTek BT keeps serving in
  screensaver; Broadcom detaches at screensaver in kindle-hid-passthrough
  (https://github.com/zampierilucas/kindle-hid-passthrough/blob/main/kindle_hid_passthrough/power_monitor.py).
- KinAMP/LARK simply hold `preventScreenSaver 1` while in the foreground. One project found
  that this makes **short power-button presses get ignored**
  (https://github.com/yzgpk2002/kindle8-offline-home/blob/main/docs/bluetooth-duplex.md).
  It also keeps the frontlight on (inference) — bad for battery in a pocket.
- KOReader: KeepAlive plugin toggles `preventScreenSaver`; AutoSuspend resets t1 every 4
  min; `powerButton 1` to suspend; `rtcWakeup` only in ReadyToSuspend.

### Unknown (audio/power) — to settle with `tools/kindle-diagnose.sh` + device tests
1. Whether `mixersink` exists and works on FW ≥ 5.19 (GStreamer 1.0), and whether a
   `gst-launch-1.0`/`gst-launch-0.10` CLI binary is present on each firmware.
2. Mixer → A2DP internals (Broadcom and MediaTek); whether Audible holds a suspend
   inhibitor.
3. Whether answering `readyToSuspend` with `deferSuspend` keeps audio playing with the
   screensaver up on current firmware (no published proof).
4. Whether the stock screensaver draws while `awesome` is SIGSTOPped.
5. Any lipc volume property; AVRCP volume.
6. Which `mixersink` formats work besides 44.1 kHz stereo S16LE.
7. On Broadcom models, whether BT audio survives the screensaver at all.

## 7. Local dev environment (reference build machine)

- macOS 15.2, Apple Silicon, Xcode clang 16, no Docker.
- `brew install zig` → Zig 0.17.0. `zig cc -target arm-linux-musleabihf -mcpu=cortex_a8 -static`
  produces static ARM EABI5 binaries; `-target arm-linux-gnueabi.2.12` and
  `arm-linux-gnueabihf.2.17` (dynamic glibc) also work if ever needed.
- Homebrew `ffmpeg` 8.0.1 is installed — used only to generate test fixtures.
- ffmpeg.org frequently times out on the first TCP connect → `curl --retry`.
- Xcode provides `llvm-nm` (`xcrun -f llvm-nm`), passed to FFmpeg's configure.
- ARM binaries cannot be executed on macOS (no qemu-user). Options if needed later:
  `brew install lima` + an Ubuntu VM with `qemu-user-static`.
- Publishing is via GitHub Releases with a MobileRead announcement; see
  `docs/RELEASING.md`.
