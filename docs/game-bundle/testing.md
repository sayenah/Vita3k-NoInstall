# Testing — play a game with no install

This fork boots a game directly from a `.pkg`/`.zip`/`.7z` (or a prepared folder) with **no install into
`ux0/app`**. This guide covers the everyday flow (a **ROMs folder** library) plus the two CLI entry
points, and the acceptance checks. Desktop targets: **Windows x64** and **Apple-Silicon macOS** (both
build in CI). Android is covered in §6.

## 1. Get a build

- **End users:** grab the latest **Release** — see `docs/game-bundle/getting-builds.md`
  (`Vita3K-NoInstall.apk` for Android, `Vita3K-NoInstall-windows-x64.zip` for Windows).
- **Testing a specific commit:** open the repo's **Actions** tab → a green **Build CI** run → **Artifacts**:
  - Windows: `vita3k-<sha>-windows-x64`
  - Apple-Silicon Mac: `vita3k-<sha>-macos-arm64`
  - Android: the APK from the `android-build` job

  (Or `gh run download <run-id> -R sayenah/Vita3k-NoInstall -n vita3k-<sha>-windows-x64`.) Unzip it. This
  is a standalone build of the fork; it does not touch your normal Vita3K install.

## 2. Prerequisites (same as any Vita3K boot)

The mount only virtualizes `app0:`/`addcont0:`. Everything else — **firmware, licenses, and saves** —
still lives in the emulator's data folder. So that folder must have:

- **PS Vita firmware** installed, and
- the game's license. You have three options, easiest first:
  1. Point the fork at an **existing Vita3K data folder** that already has firmware, licenses, and saves.
  2. Set a **License folder** (§5) with the game's `.rif` files — they're copied to `ux0/license`
     automatically at launch. `tools/build-license-folder.py` builds this folder from NoPayStation TSVs.
  3. Play a **self-contained NoNpDrm dump** (a `.pkg`, or an archive of a decrypted dump) — its license
     (`work.bin`/`.rif`) is extracted to `ux0/license` automatically.

> Why the license matters: boot decrypts the eboot/modules with the rif key via `get_license` reading
> `ux0/license/...` on the real filesystem. If it's missing, boot logs a warning and you get a black
> screen / module-load errors.

## 3. The easy path — a ROMs folder

1. Put your games (each a `.pkg`, or a `.zip`/`.7z` of a NoNpDrm dump) anywhere in one folder.
   Sub-folders are fine — the scan is recursive.
2. **Set the folder:** desktop, right-click the games list → **Set ROMs Folder…**; Android, the apps-list
   overflow menu → **Set ROMs Folder…**.
3. The games appear in the library with their real names and icons. **Tap one to play** — it decrypts to a
   temp folder, mounts it read-only, boots, and cleans up the temp folder when you quit.

Notes:
- A raw `.pkg` shows the **default icon** (its real icon is inside the encrypted PFS); a `.zip`/`.7z` of a
  decrypted dump shows the real icon.
- Two games that share a title id (e.g. a base game and a fan translation in sibling sub-folders) are
  disambiguated in the list by their sub-folder name.
- If a file can't be read, the scan reports its name and skips it; the rest of the list still loads.

## 4. CLI: play a single file (`--play-pkg`)

For scripting/automation or a quick one-off, boot a single archive without adding it to a library:

```
# Windows
Vita3K.exe --play-pkg "C:\games\Persona 4 Golden.pkg"

# macOS (run the binary inside the .app so you can pass args)
./Vita3K.app/Contents/MacOS/Vita3K --play-pkg ~/games/persona4.zip
```

Accepts a NoNpDrm `.pkg`, or a `.zip`/`.7z` containing either a `.pkg` or an already-decrypted `app/`
tree. Updates/DLC/licenses are still pulled from the configured folders (§5) if set.

## 5. DLC, updates, and licenses (side folders)

All three are optional, set the same way as the ROMs folder, and matched to a game by its **title id**.
They're consumed at launch — none of them add their own row to the games list.

- **DLCs folder** — set it to a folder holding, for a game with title id `PCSE00123`, any of:
  - a `PCSE00123/` sub-folder of DLC `.pkg`s (or decrypted addcont trees),
  - a `PCSE00123.zip` / `PCSE00123.7z` of the same,
  - loose `.pkg` files (matched by the pkg's embedded title id).
- **Updates folder** — same three layouts. Only real update pkgs (category `gp`) are applied; the highest
  version wins. The update is merged over the game's `app/` before boot.
- **License folder** — a `PCSE00123/<CONTENTID>.rif` tree, or a `license.zip` of that tree. The matching
  game's rifs are copied to `ux0/license/<TITLEID>/` at launch. This is the folder
  `tools/build-license-folder.py` produces.

## 6. Dev/testing: mount a prepared folder (`--bundle`)

`--bundle` points at an **already-decrypted** game folder and mounts it in place (no decrypt, no temp,
nothing deleted). Useful for isolating the mount from the decrypt path.

Layout:

```
BundleDir/
├── vita3k_bundle.json
└── app/                     <-- the CONTENTS of ux0/app/<TITLEID>/ (eboot.bin, sce_sys/, …)
```

`vita3k_bundle.json` (minimum — set your real title id):

```json
{ "version": 1, "title_id": "PCSE00123", "content_id": "", "category": "gd", "has_patch": false, "dlc": [] }
```

Make one from an installed game (an installed `ux0/app/<TITLEID>/` **is** a decrypted tree):

```bash
tid=PCSE00123; data=~/path/to/Vita3K/data; dest=~/bundles/MyGame
mkdir -p "$dest/app"
cp -a "$data/ux0/app/$tid/." "$dest/app/"
printf '{ "version":1, "title_id":"%s", "content_id":"", "category":"gd", "has_patch":false, "dlc":[] }\n' "$tid" > "$dest/vita3k_bundle.json"
```

```powershell
# Windows (PowerShell)
$tid="PCSE00123"; $data="C:\path\to\Vita3K\data"; $dest="C:\bundles\MyGame"
New-Item -ItemType Directory -Force "$dest\app" | Out-Null
Copy-Item "$data\ux0\app\$tid\*" "$dest\app\" -Recurse -Force
'{ "version":1, "title_id":"'+$tid+'", "content_id":"", "category":"gd", "has_patch":false, "dlc":[] }' | Set-Content "$dest\vita3k_bundle.json"
```

Add `addcont/<CONTENTID>/...` folders (and list the content ids in `"dlc"`) for DLC. Launch:

```
Vita3K.exe --bundle "C:\bundles\MyGame"
./Vita3K.app/Contents/MacOS/Vita3K --bundle ~/bundles/MyGame
```

### Android (adb / ES-DE)

The `.Emulator` activity is exported and self-initializes native, so this works from a cold start:

- **Play an archive with no install:** tap a game in the ROMs library (§3), or launch via a file path.
- **Mount a prepared bundle folder** (the `--bundle` equivalent): pass the `bundle_path` extra (or a
  `VIEW` `file://` data URI):

  ```
  adb shell am start -n org.vita3k.emulator/org.vita3k.emulator.Emulator \
    -e bundle_path /sdcard/bundles/MyGame
  ```

- A debug APK's package is `org.vita3k.emulator.debug` (check with `adb shell pm list packages | grep
  vita3k`). Make sure the emulator isn't already mid-game — the fresh-launch path is what reads it.
- **ES-DE:** point the Vita3K launch command at `org.vita3k.emulator/.Emulator` with
  `-e bundle_path <ROM path>`.

## 7. Acceptance checks

- [ ] **Boots** from the ROMs folder / `--play-pkg` / `--bundle`; the game runs. Delete or move the source
      afterwards — saves under `ux0/user` are untouched.
- [ ] **No permanent install** — after playing, there is no new folder under `ux0/app`, and `<cache>/pkgplay`
      is gone (the temp tree is deleted on exit).
- [ ] **Saves work** — create a save, confirm it lands in the data folder's `ux0/user/.../savedata`.
- [ ] **Read-only** — any write/rename/delete the game attempts under `app0:` returns `EROFS` and the game
      keeps running (no crash/corruption).
- [ ] **FMV** — a video-heavy title plays its movies (SceAvPlayer routed through the mount).
- [ ] **DLC** — a title with DLC in the DLCs folder detects and loads it.
- [ ] **Update** — a title with an update in the Updates folder boots patched.
- [ ] **License folder** — with only a License folder set (no manual `ux0/license` copy), a game boots.

## 8. Troubleshooting

- **"Failed to play … / could not determine the game's title id"** → the archive isn't a NoNpDrm pkg / a
  decrypted `app/` tree, or its `param.sfo` is unreadable.
- **"Failed to mount Game Bundle …"** (`--bundle`) → `vita3k_bundle.json` missing/invalid or `version` != 1;
  check the JSON and that the folder contains `app/`.
- **Boots to a black screen / module-load errors** → firmware not installed, or the game's license isn't in
  `ux0/license/<TITLEID>/` (see §2 — set a License folder, or use a self-contained dump).
- **A ROM is missing from the list** → its file couldn't be read; the scan logs it as unreadable. Run from a
  terminal to see the per-file reason.
- **DLC/update not applied** → check the file is under the right folder and its embedded title id matches the
  game; updates must be category `gp`.
- Logging: run from a terminal to see `Prepared [...] for play-without-install`, `Mounted Game Bundle …`,
  and any decrypt/mount errors.
