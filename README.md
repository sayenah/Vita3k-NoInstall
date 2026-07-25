<div align="center">

# 🎮 Vita3K‑NoInstall

### Play your PS Vita games straight from `.zip` / `.7z` / `.pkg` files — no install step, ever.

[![Build CI](https://github.com/sayenah/Vita3k-NoInstall/actions/workflows/c-cpp.yml/badge.svg?branch=feature%2Fgame-bundle)](https://github.com/sayenah/Vita3k-NoInstall/actions/workflows/c-cpp.yml)
[![Latest release](https://img.shields.io/badge/release-latest-blue)](https://github.com/sayenah/Vita3k-NoInstall/releases/latest)
[![License: GPL v2](https://img.shields.io/badge/license-GPLv2-orange)](./COPYING.txt)
[![Upstream](https://img.shields.io/badge/fork%20of-Vita3K-red)](https://github.com/Vita3K/Vita3K)

**A friendly fork of [Vita3K](https://github.com/Vita3K/Vita3K), the experimental PS Vita emulator —
with the install ritual removed.**

[![Download for Windows](https://img.shields.io/badge/⬇%20Windows%20x64-Vita3K--NoInstall--windows--x64.zip-2ea44f?style=for-the-badge)](https://github.com/sayenah/Vita3k-NoInstall/releases/latest/download/Vita3K-NoInstall-windows-x64.zip)
[![Download for Android](https://img.shields.io/badge/⬇%20Android-Vita3K--NoInstall.apk-2ea44f?style=for-the-badge&logo=android&logoColor=white)](https://github.com/sayenah/Vita3k-NoInstall/releases/latest/download/Vita3K-NoInstall.apk)

</div>

---

## Why?

Stock Vita3K asks you to **install** every game: each `.pkg` gets unpacked into the emulator's data
folder, so you keep two copies (the pkg *and* the install), your games hide behind cryptic
`PCSE00123` folder names, and front-ends like ES-DE can't just launch a file.

**Vita3K‑NoInstall boots the file directly.** Keep one nicely-named archive per game, anywhere you
like — internal storage, SD card, a NAS folder — and just press play.

## How it works

| | |
|---|---|
| 🗜️ **1. You pick a game** | A NoNpDrm `.pkg`, or a `.zip`/`.7z` of a decrypted dump — from the built-in library, ES-DE, a file manager, or the command line. |
| 🔓 **2. It unpacks to a private temp folder** | Decrypted on the fly (updates merged in, DLC attached, licenses applied — see below). |
| 🎮 **3. It boots from a read-only mount** | The game sees a normal `app0:`; every write attempt is safely refused. **When you quit, the temp folder is deleted.** Nothing is ever installed. |

Your **saves, trophies, and licenses live in the normal Vita3K data folder** the whole time — moving
or deleting a game file never touches them.

## Features

- 📚 **ROMs library** — point the emulator at your games folder(s) and every `.zip`/`.7z`/`.pkg`
  shows up with its real title and icon. **Multiple folders supported** (e.g. internal + SD card);
  sub-folders are scanned too.
- 🕹️ **ES-DE ready** — launch games by file path from ES-DE on **Android and Windows** (setup
  snippet below). The launcher auto-detects what it's handed, so any front-end that passes a path works.
- 🩹 **Updates folder** — drop update pkgs in one folder; the newest matching patch is applied
  automatically at launch. No install.
- 🧩 **DLCs folder** — same idea for DLC: pkgs, folders, or per-game zips, matched by title id and
  mounted at launch.
- 🔑 **License folder** — keep your `.rif` licenses in one tree (there's a
  [tool](./tools/build-license-folder.py) that builds it from NoPayStation `.tsv` files) and every
  game/update/DLC key is applied automatically.
- 🔄 **Self-updating** — every Monday the fork rebases itself onto the newest official Vita3K and
  publishes a fresh build. There's also a one-click button ([how-to](./docs/game-bundle/getting-builds.md)).
- 🖥️ **All Vita3K goodness** — this fork *adds* features; normal installs, settings, save data, and
  compatibility are exactly upstream Vita3K.

## Quick start

> **Prerequisite (one-time):** like stock Vita3K, you need the **PS Vita firmware** installed and your
> games' **licenses** available — easiest is pointing the fork at an existing Vita3K data folder, using
> the License folder, or playing self-contained NoNpDrm dumps (their license is applied automatically).
> Details in the [testing guide](./docs/game-bundle/testing.md).

**Windows**
1. Download [`Vita3K-NoInstall-windows-x64.zip`](https://github.com/sayenah/Vita3k-NoInstall/releases/latest/download/Vita3K-NoInstall-windows-x64.zip), extract, run `bin/Vita3K.exe`.
2. Right-click the games list → **ROMs Folders → Add Folder…** and pick where your games live.
3. Double-click a game. That's it.

**Android**
1. Download and install [`Vita3K-NoInstall.apk`](https://github.com/sayenah/Vita3k-NoInstall/releases/latest/download/Vita3K-NoInstall.apk).
2. Apps list → **⋮ → ROMs Folders… → Add Folder…**.
3. Tap a game. That's it.

**Command line** (Windows/Linux/macOS)

```
Vita3K --play-pkg "D:\Games\Persona 4 Golden.zip"
```

## ES-DE setup

The stock ES-DE Vita3K config expects installed games (`.psvita` title-id files) — replace the launch
command in your custom system entry:

**Windows** (`es_systems.xml`):
```xml
<command label="Vita3K NoInstall">%EMULATOR_VITA3K% --play-pkg %ROM%</command>
```

**Android**:
```xml
<command label="Vita3K NoInstall">%EMULATOR_VITA3K%%EXTRA_archive_path%=%ROM%</command>
```

Add `.zip`, `.7z` and `.pkg` to the system's extension list, and ES-DE launches your games end-to-end.

## Downloads & staying current

Grab the newest build any time from **[Releases → latest](https://github.com/sayenah/Vita3k-NoInstall/releases/latest)**.
It refreshes automatically every Monday when upstream Vita3K changes; the
[builds guide](./docs/game-bundle/getting-builds.md) shows the one-click "build now" button and what to
do if an update ever needs a human. Linux and macOS builds are available as
[CI artifacts](https://github.com/sayenah/Vita3k-NoInstall/actions/workflows/c-cpp.yml).

## For developers

| Doc | What's inside |
|---|---|
| [brief.md](./docs/game-bundle/brief.md) | Architecture: the read-only mount, decrypt-to-temp pipeline, verified code map |
| [testing.md](./docs/game-bundle/testing.md) | How to test every path: ROMs library, CLI flags, adb/ES-DE, acceptance checks |
| [updating.md](./docs/game-bundle/updating.md) | Rebasing the fork onto newer upstream Vita3K |
| [getting-builds.md](./docs/game-bundle/getting-builds.md) | The CI/release automation, for users and maintainers |
| [building.md](./building.md) | Building from source (unchanged from upstream) |

The fork is a small, rebase-friendly patch series: almost all code lives in new files
(`io/bundle.*`, `packages/archive_*`, `app/roms_list.cpp`), with minimal hooks in upstream files.

## Credits & license

All emulation is the work of the amazing **[Vita3K team](https://github.com/Vita3K/Vita3K)** — support
them on [ko-fi](https://ko-fi.com/vita3k). This fork only changes *how games are loaded*.

Licensed under **[GPLv2](./COPYING.txt)**, same as upstream.

> **Note:** this project does not enable piracy. Play games you own — dump them from your own Vita
> with [NoNpDrm](https://github.com/TheOfficialFloW/NoNpDrm) or
> [FAGDec](https://github.com/CelesteBlue-dev/PSVita-RE-tools/tree/master/FAGDec/build). PlayStation,
> PlayStation Vita and PlayStation Network are registered trademarks of Sony Interactive Entertainment
> Inc. This emulator is not related to or endorsed by Sony.
