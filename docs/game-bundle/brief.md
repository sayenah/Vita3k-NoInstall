# Vita3K Fork: Play PS Vita Games With No Install

Design + implementation reference. Written against upstream `master`; the code map in §5 was verified
against commit `24a401f` (2026-07-05). This in-repo copy is the **source of truth**.

> **History note.** The original brief proposed a single persistent *STORE-mode zip per game* ("Game
> Bundle") produced by a one-time **import**, with direct-from-pkg playback deferred as the top
> risk. What actually shipped is different and simpler: there is no import step and no persistent
> per-game archive. You keep your `.pkg`/`.zip`/`.7z` files in folders; each launch **decrypts/unpacks
> the game to a temporary directory, mounts that read-only, boots, and deletes it on exit.** The
> feared random-access PFS decryptor was avoided by doing a full one-shot decrypt-to-temp (reusing the
> shipping `install_pkg`). The pieces the brief got right — the read-only mount interface, the IO
> op-surface routing, and the code map (§5) — are exactly what the shipped design is built on. Sections
> below describe what is **actually implemented**.

## 1. Problem & Approach

**Problem.** Stock Vita3K requires installing `.pkg` files into `<data>/ux0/app/<TITLEID>/`. That means:
duplicate storage (source pkg + installed copy), ugly nine-character folder names instead of game
names, a manual install ritual per game, and no easy way to offload a game (ES-DE on Android can't
orchestrate install/uninstall around a launch).

**Approach.** Boot a game directly from a nicely-named archive or folder, with **no permanent install**:

1. A read-only **mount** virtualizes the `app0:` and `addcont0:` devices. Reads/stats/dir-listings under
   those roots are served from the mounted game tree; every mutating op returns `EROFS`. Everything else
   — firmware (`vs0:`, `os0:`), saves (`ux0/user`), licenses (`ux0/license`) — stays on the real host
   filesystem, untouched.
2. To boot an encrypted `.pkg` (or an archive of one), the game is **decrypted/unpacked to a private
   temp directory**, that directory is mounted, and it is **deleted when the game stops** (`io_deinit`).
   Nothing persists on disk between plays except your original source files.
3. Optional **DLC, game updates, and licenses** are pulled in at launch from three configurable side
   folders, matched to the game by title id (§3).

Runtime UX: one pretty-named file (or folder) per game, no install, single copy on disk, trivially
movable/offloadable. The cost is a short decrypt/unpack at each boot instead of a one-time install.

## 2. Two Mount Entry Points

Both build a read-only mount (`io.mount`, a `shared_ptr<BundleMount>`) and boot through the normal path.

- **`--play-pkg <path>` (desktop) / library tap / Android play-archive — the real play path.**
  `mount_pkg_for_play` (`packages/src/pkg.cpp:936`) accepts:
  - a raw self-contained **NoNpDrm `.pkg`** (the zRIF is derived from `sce_sys/package/work.bin`, or
    from a `.rif` placed via the License folder);
  - a **`.zip`/`.7z` containing a `.pkg`** (the pkg is extracted, then decrypted);
  - a **`.zip`/`.7z` containing an already-decrypted `app/` tree** (unpacked directly; may also carry
    `vita3k_bundle.json`, an `addcont/` tree, and `work.bin`/`*.rif`).

  It decrypts/unpacks into `<cache>/pkgplay`, overlays the game's latest update, brings in its DLC as
  `addcont0:` content, copies its licenses to `ux0/license`, writes a synthetic `vita3k_bundle.json`,
  mounts the temp tree (directory backend), and returns the title id to boot. The temp tree is owned by
  the mount (`BundleMount::owned_temp`) and removed in `io_deinit` when the game stops.

- **`--bundle <dir>` (dev/testing) — mount a prepared directory.**
  Mounts an already-decrypted **Game Bundle directory** (a folder with `vita3k_bundle.json` + `app/`,
  optionally `addcont/`) via `open_directory_backend` and boots it (`main.cpp:257`). No decryption, no
  temp, nothing deleted — it points at your folder in place. Android exposes the same via the
  `bundle_path` intent extra (or a `VIEW` `file://` data URI) on the exported `.Emulator` activity.

A synthetic `app::AppEntry` is pushed into the in-memory apps list so `set_app_info` → `load_app`
resolves to the mount; the real game title is loaded from the bundle's `param.sfo` at boot.

## 3. The Library + Side Folders

Four folders are configurable in Settings (desktop Qt: right-click menu "Set … Folder…";
Android: the apps-list overflow menu). All four are optional and matched to a game by **title id**.

- **ROMs folder** (`cfg.roms_folder`). Scanned **recursively** for `.zip`/`.7z`/`.pkg`
  (`app/src/roms_list.cpp:scan_roms`); each becomes a games-list row with its real title and extracted
  `icon0.png`. Nested sub-folders are supported; two archives that share a title id (e.g. a base game and
  a translation in sibling sub-folders) are disambiguated by appending their sub-folder name. Tapping a
  row launches it through `mount_pkg_for_play`. Metadata is read cheaply without decrypting: from an
  archive's `sce_sys/param.sfo` member, or a raw pkg's unencrypted info-section `param.sfo`
  (`read_pkg_param_sfo`); a raw pkg's icon lives inside the encrypted PFS, so those show the default icon.

- **DLCs folder** (`cfg.dlc_folder`). At launch, the game's DLC is decrypted into `<temp>/addcont`.
  Accepted layouts, matched to the launched title id:
  - a `<TITLEID>/` sub-folder holding DLC `.pkg`s and/or already-decrypted addcont trees;
  - a `<TITLEID>.zip` / `<TITLEID>.7z` of the same;
  - loose `.pkg` files anywhere in the folder, matched by the pkg header's embedded title id.
  DLC shipped inside the game archive (an `addcont/` folder next to `app/`) is also picked up.

- **Updates folder** (`cfg.updates_folder`). At launch, the game's update patch is decrypted and
  **merged over `<temp>/app`** (overwrite-existing). Same three layouts as DLC. Only pkgs with category
  `gp` (a real update) are applied; if several match, the **highest `app_version` wins** (Vita patches
  are cumulative).

- **License folder** (`cfg.license_folder`). At launch, the game's `.rif` licenses (base + updates + DLC
  all share one title id) are copied to `ux0/license/<TITLEID>/` so boot's `get_license` and the
  update/DLC decrypt can find every key — no manual copy into `ux0`. Accepted:
  - a `<TITLEID>/` folder of `.rif` files (exactly the tree `tools/build-license-folder.py` produces);
  - a `license.zip` holding the same `<TITLEID>/<CONTENTID>.rif` tree.

  Additionally, when a decrypted-dump archive is played, any `work.bin`/`*.rif` inside it is copied to
  `ux0/license` automatically (case-insensitive, recursive — this is what made Android dumps work).

## 4. Bundle Format & Manifest

The mount is fed a directory tree with this shape (produced in `<temp>` by `mount_pkg_for_play`, or
prepared by hand for `--bundle`):

```
<bundle root>/
├── vita3k_bundle.json   # {"version":1,"title_id":"PCSE00123","content_id":"...",
│                        #  "category":"gd","has_patch":false,"dlc":[]}
├── app/                 # decrypted game tree == contents of ux0/app/<TITLEID>/, update merged in
├── addcont/<CONTENTID>/ # one decrypted tree per DLC == ux0/addcont/<TITLEID>/<CONTENTID>/
└── (licenses are NOT kept here — they are copied to the real ux0/license at launch)
```

`vita3k_bundle.json` is **required** by the directory backend and version-checked (`version` must be 1);
`title_id` is the only field the backend truly needs. `map_ux0_path` translates a post-`translate_path`
`app/<TITLEID>/…` path to the bundle key `app/…`, and `addcont/<TITLEID>/…` to `addcont/…`. The manifest
parser (`bundle::parse_manifest`) is a minimal, tolerant reader for this exact flat schema — not a
general JSON parser (we control the emitter). Paths inside the tree resolve case-insensitively
(`resolve_icase`) so wrong-case requests work on case-sensitive hosts, and never escape the root.

## 5. Verified Code Map (where everything hooks)

**The IO funnel is narrow.** Host paths are built by `io.cpp` + `device.cpp` (`construct_emulated_path`),
with module loads via `module_parent`'s `translate_path` and FMV via `SceAvPlayer`.

- **Op surface routed** (`io/src/io.cpp`): `open_file`, `stat_file`, `stat_file_by_fd`, `open_dir`,
  `read_dir` serve from the mount when the path maps to a bundle key; `write_file`/`truncate_file`,
  `remove_file`, `rename`, `create_dir`, `remove_dir` return `SCE_ERROR_ERRNO_EROFS` (`io/io.h`) for
  paths under `app0:`/`addcont0:`. Bundle-backed fds live in the existing `std_files`/`dir_entries`
  maps via a second `FileStats`/`DirStats` constructor that holds an `EntryReader`/`DirReader` instead
  of a host file (`io/state.h`); `state_functions.cpp` routes read/seek/tell/truncate to the reader +
  an fd cursor. `io_deinit` deletes `owned_temp` and clears `io.mount`.
- **Per-boot device mapping**: `init_device_paths` sets `device_paths.app0 = "app/" + io.app_path` and
  `addcont0 = "addcont/" + io.addcont`; for games `io.app_path == io.addcont == title_id`.

**Raw-`fs::` stragglers routed through the bundle** (host-path touches that bypass emulated IO):

| Site | What it does | Hook |
|---|---|---|
| `io.cpp` `vfs::read_app_file` | param.sfo + loader whole-file reads | now takes `IOState&`; `try_read_ux0_file` first, else host |
| `module_parent.cpp` `load_module` | loads `app0:…` modules | `try_read_ux0_file`; also **skips the host-FS case-insensitive fallback** when the mount covers the path (the Android fix) |
| `interface.cpp:506` (`add_preload_module`) | existence probe for app-supplied `sce_module/*.suprx` | `try_exists_ux0` |
| `app/src/app.cpp:277` (`prepare_game_launch_overlay`) | renderer wants a **host path** to `pic0.png` | extracts pic0 from the bundle to a cache file, points the string there |
| `SceAppUtil.cpp` `is_addcont_exist` | DLC detection (`exists && !is_empty`) | `try_exists_ux0` (dir "exists" == non-empty, matching the host check) |
| `SceAvPlayer.cpp` `sceAvPlayerAddSource` | FMV opened as a host file when the game gives no IO callbacks | extracts the media from the bundle to a temp cache file |
| `SceAppMgr.cpp` `_sceAppMgrLoadExec` | loads an exec from the app tree | `read_app_file(io, …)` |

- **Decrypt reuses existing code**: `install_pkg` (`pkg.cpp`) gained a `temp_root` parameter — when set,
  it decrypts the pkg into `temp_root/app` and **skips** the `ux0/app` copy, but still writes the
  license rif to the real `ux0/license`. With no zRIF supplied it derives one from `work.bin`.
- **Archive layer** (`packages/archive_read.*`, `archive_7z.*`, vendored `external/lzma-sdk/`): zip via
  miniz (already in-tree), 7z via the LZMA SDK. Helpers: detect a decrypted tree vs a pkg, extract-all,
  extract-first-pkg, read one member in memory (metadata scans).
- **Do NOT use `-d`/`--deleted-id`** — it deletes savedata and shader cache. Nothing in the play path
  uses it; only the mount's own `owned_temp` is removed.
- **CI**: `.github/workflows/c-cpp.yml` builds desktop (windows-x64/arm64, linux-appimage, macos-x64/
  arm64) and android (APK) on every push; it also has a `workflow_dispatch` trigger.

## 6. Data-Safety Invariants (test them)

- Nothing under `ux0/user` (saves) or `ux0/license` is ever **written or deleted** by the mount. (The
  License folder only **adds** `.rif` files to `ux0/license`; it never removes anything.)
- Everything the play path decrypts lives under `<cache>/pkgplay` and is deleted on exit; deleting or
  moving a source archive never touches saves, trophies, or licenses.
- The mount is read-only end to end: any write/rename/remove/create/truncate under `app0:`/`addcont0:`
  returns `EROFS` and the game keeps running.

## 7. Constraints & Fork Hygiene

- Simplest thing that works; do not touch code unrelated to the task; no speculative abstractions beyond
  the mount interface.
- **Rebase strategy:** new functionality in new files (`io/bundle.*`, `packages/archive_*`,
  `app/roms_list.cpp`, `external/lzma-sdk/`, these docs); existing-file diffs limited to the routing
  hooks in §5. See `docs/game-bundle/updating.md`.
- **License:** Vita3K is GPLv2 — the fork and any distributed APKs must remain GPL-compliant. The
  vendored LZMA SDK is public-domain, compatible.

## 8. Parked / Not Built

- **Persistent STORE-zip backend** (the original brief's "P1"): a zip reader that `pread`s members off a
  single host fd, so the game plays straight from the `.zip` with **no temp extraction**. Not built —
  decrypt/unpack-to-temp does the job at the cost of transient disk + a per-boot unpack. This is the
  main thing that would make large games boot faster and use less scratch space.
- **Seekable compression** (zstd-seekable / chunked-deflate) for a compressed bundle format — the
  manifest carries a `version` field for this.
- **A true random-access PFS reader** (decrypt pkg sectors on demand instead of all at once) — the
  original top risk; unnecessary given decrypt-to-temp.
