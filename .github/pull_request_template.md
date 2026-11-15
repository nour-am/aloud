## What and why

<!-- What does this change, and what problem does it solve? Link the issue: Fixes #123 -->

## How it was tested

- [ ] `make test` passes
- [ ] UI changes: `make sim` screenshots checked (attach before/after if the look changed)
- [ ] Tested on a Kindle (model + firmware): <!-- e.g. Paperwhite 5, 5.19.2 — or "not tested on device" -->

## Checklist

- [ ] One focused change; no unrelated formatting
- [ ] Every syscall and FFmpeg return value is checked
- [ ] No busy polling, no wake-ups while paused, no allocations in the audio path
- [ ] Docs updated if behaviour changed (`README.md`, `docs/`)
