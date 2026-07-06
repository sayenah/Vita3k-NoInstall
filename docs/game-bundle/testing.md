# P0 testing — boot a game from a folder via `--bundle`

P0 ships a **directory backend**: a "bundle" is a plain folder on disk (zip support is P1). This guide
walks through booting an already-decrypted game from such a folder with **no install into `ux0/app`**,
and running the P0 acceptance checks. Targets: **Windows x64** and **Apple-Silicon macOS** (both build
in CI; Intel-mac CI is currently broken at runner setup).

## 1. Get a build

Download the CI artifact for your device from the repo's **Actions** tab → pick a green **Build CI**
run → scroll to **Artifacts**:

- Windows: `vita3k-<sha>-windows-x64` (~36 MB)
- Apple-Silicon Mac: `vita3k-<sha>-macos-arm64` (~57 MB)

(Or with the `gh` CLI: `gh run download <run-id> -R sayenah/Vita3k-NoInstall -n vita3k-<sha>-windows-x64`.)
Unzip it. This is a standalone build of the fork; it does not touch your normal Vita3K install.

## 2. Prerequisites (same as any Vita3K boot)

The mount only virtualizes `app0:`/`addcont0:`. Everything else — **firmware, the game's license,
and saves** — still lives in the emulator's data folder. So that folder must have:

- **PS Vita firmware** installed, and
- the game's license at `ux0/license/<TITLEID>/<CONTENTID>.rif`.

Easiest: point the fork at your **existing Vita3K data folder** (Settings → the Vita3K data/pref
folder), which already has firmware, licenses and saves. Alternatively, install firmware + the game
once into a fresh data folder for the fork.

> Why the license must already be there: boot decrypts the eboot/modules with the rif key via
> `get_license` reading `ux0/license/...` on the real filesystem. The importer (P2) will pre-place
> rifs automatically; for this P0 folder test you reuse the one from a normal install.

## 3. Make a test bundle folder from an installed game

An installed game at `ux0/app/<TITLEID>/` **is** a decrypted game tree — reuse it.

```
BundleDir/
├── vita3k_bundle.json
└── app/                     <-- copy the CONTENTS of ux0/app/<TITLEID>/ in here
    ├── eboot.bin
    ├── sce_sys/
    └── ...
```

`vita3k_bundle.json` (minimum — set your real title id):

```json
{ "version": 1, "title_id": "PCSE00123", "content_id": "", "category": "gd", "has_patch": false, "dlc": [] }
```

Windows (PowerShell), from an existing install:

```powershell
$tid  = "PCSE00123"
$data = "C:\path\to\Vita3K\data"          # folder that contains ux0\
$dest = "C:\bundles\MyGame"
New-Item -ItemType Directory -Force "$dest\app" | Out-Null
Copy-Item "$data\ux0\app\$tid\*" "$dest\app\" -Recurse -Force
'{ "version":1, "title_id":"'+$tid+'", "content_id":"", "category":"gd", "has_patch":false, "dlc":[] }' |
  Set-Content "$dest\vita3k_bundle.json"
```

macOS/Linux shell equivalent:

```bash
tid=PCSE00123; data=~/path/to/Vita3K/data; dest=~/bundles/MyGame
mkdir -p "$dest/app"
cp -a "$data/ux0/app/$tid/." "$dest/app/"
printf '{ "version":1, "title_id":"%s", "content_id":"", "category":"gd", "has_patch":false, "dlc":[] }\n' "$tid" > "$dest/vita3k_bundle.json"
```

**DLC (optional):** add `addcont/<CONTENTID>/...` folders (contents of
`ux0/addcont/<TITLEID>/<CONTENTID>/`) and list the content ids in `"dlc"`. The DLC's rif must also be
in `ux0/license/<TITLEID>/`.

## 4. Launch

Windows:

```
Vita3K.exe --bundle "C:\bundles\MyGame"
```

macOS (run the binary inside the .app so you can pass args):

```
./Vita3K.app/Contents/MacOS/Vita3K --bundle ~/bundles/MyGame
```

It should mount the folder, register the title, and boot straight into the game — no entry in the
normal games list, no copy under `ux0/app`.

### Android (adb / ES-DE)

The `.Emulator` activity accepts a `bundle_path` extra (or a `VIEW` `file://` data URI) and boots it
directly. It's already exported and self-initializes native, so this works from a cold start.

Prep on the device:

- Install the fork APK (`android/app.apk`), launch it once so firmware is installed and the game's
  license exists (or reuse your existing Vita3K-Android data folder).
- Put a bundle folder on storage, e.g. `/sdcard/bundles/MyGame/` with `app/` + `vita3k_bundle.json`
  (same layout as above). An installed Android game at `<storage>/ux0/app/<TITLEID>` is a decrypted
  tree you can copy. (The app holds all-files access, so native reads the plain path directly.)

Launch:

```
adb shell am start -n org.vita3k.emulator/org.vita3k.emulator.Emulator \
  -e bundle_path /sdcard/bundles/MyGame
```

- Add `-e title_id PCSE00123` if you want. A debug APK's package is `org.vita3k.emulator.debug`
  (check with `adb shell pm list packages | grep vita3k`).
- Make sure the emulator isn't already mid-game — the fresh-launch path is what reads the bundle.
- **ES-DE:** point the Vita3K launch command at `org.vita3k.emulator/.Emulator` with
  `-e bundle_path <ROM path>`.

## 5. P0 acceptance checks

- [ ] **Boots** from the folder; the game runs. Delete/rename `BundleDir` afterwards — saves under
      `ux0/user` are untouched.
- [ ] **Saves work** — create a save, confirm it lands in the data folder's `ux0/user/.../savedata`,
      not in the bundle.
- [ ] **Read-only** — the bundle is a read-only mount; any write/rename/delete the game attempts under
      `app0:` returns `EROFS` and the game keeps running (verify it doesn't crash or corrupt).
- [ ] **FMV** — a video-heavy title plays its movies (SceAvPlayer routed through the bundle).
- [ ] **DLC** — a title with `addcont/` in the bundle detects and loads its DLC.

## Troubleshooting

- **"Failed to mount Game Bundle …"** → `vita3k_bundle.json` missing/invalid or `version` != 1. Check
  the JSON and that the folder contains `app/`.
- **Boots to a black screen / module load errors** → firmware not installed, or the game's
  `ux0/license/<TITLEID>/<CONTENTID>.rif` is missing from the data folder (see §2).
- **"Could not find app '<TITLEID>' in apps list"** → the manifest `title_id` must match the game's
  real title id (the `PCSE…`/`PCSB…` folder name under `ux0/app`).
- Logging: run from a terminal to see `LOG_INFO("Mounted Game Bundle …")` and any mount errors.
