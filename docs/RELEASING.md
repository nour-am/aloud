# Releasing Aloud

Aloud ships in two places:

| Where | What | Why |
|---|---|---|
| [GitHub Releases](https://github.com/NoureldinAmer/aloud/releases) | The install zip + `SHA256SUMS.txt` | Canonical download, built by CI from a tag |
| [MobileRead, Kindle Developer's Corner](https://www.mobileread.com/forums/forumdisplay.php?f=150) | One announcement thread, updated each release | Where Kindle modders look for new apps |

Link to [kindlemodding.org](https://kindlemodding.org) for jailbreak, hotfix and KUAL
instructions instead of copying them.

## 1. Before tagging

From a clean `main`:

```bash
make test
make package && unzip -l dist/aloud-*.zip
```

The zip must contain `documents/Aloud.sh`, `extensions/aloud/` (`config.xml`, `menu.json`,
`aloud.sh`, `bin/aloud`, `bin/aloud-sink-armel`, `bin/aloud-sink-armhf`, `fonts/*.ttf` and
`fonts/NotoSansCJK-*.otf` with their OFL texts, `koreader/aloud.koplugin/`, `LICENSE.txt`) and `audiobooks/README.txt`.

Privacy sweep over everything that will be pushed (should print nothing unexpected):

```bash
git grep -n -E "/Users/|/home/" -- .
git grep -n -E "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}" -- . ':!LICENSE' ':!CODE_OF_CONDUCT.md'
```

Run [DEVICE-TESTING.md](DEVICE-TESTING.md) on a real Kindle before calling a release stable.

## 2. Tag

1. Bump `VERSION` in the `Makefile` and `<version>` in `package/config.xml`.
2. Add `docs/release-notes/vX.Y.Z.md` and a `CHANGELOG.md` entry.
3. Commit, then tag and push:

```bash
git tag -a vX.Y.Z -m "Aloud X.Y.Z"
git push origin main vX.Y.Z
```

`.github/workflows/release.yml` checks the tag matches both version strings, runs the
tests, builds the zip and publishes the release with the notes file and checksums. Never
replace an asset on a published release; ship a new version instead.

## 3. Announce

Edit the first post of the MobileRead thread: new version, link to the release, and what
changed. Don't start a new thread per release.

## 4. Afterwards

- Device reports are mostly model/firmware combinations not in DEVICE-TESTING.md. Record
  what they teach in `docs/RESEARCH.md` with sources, and update the model table in the
  README.
- Never ask anyone to post an unredacted diagnostic file, serial number, or library
  listing in public.
