# Vita3K Fork: Boot Games Directly From Nicely-Named Archives ("Game Bundles")

Implementation brief. Written against upstream `master`; verified against commit `24a401f`
(2026-07-05). This in-repo copy is the **source of truth** — the standalone brief that seeded it
has been superseded. Corrections applied during P0 kickoff are marked **[CORRECTED]**; verified
findings are consolidated in §9.

## 1. Problem & Decision

**Problem.** Stock Vita3K requires installing `.pkg` files into `<data>/ux0/app/<TITLEID>/`. That means:
duplicate storage (source pkg + installed copy), ugly nine-character folder names instead of game
names, a manual install ritual per game, and no easy way to offload a game (ES-DE on Android can't
orchestrate install/uninstall around a launch).

**Decision: implement a read-only "Game Bundle" mount — a STORE-mode zip of the already-decrypted
game — as v1. Defer direct-from-pkg playback to a later phase behind the same interface.**

Rationale:

- Vita pkgs have **two** encryption layers: AES-128-CTR on the container (random-access friendly,
  code already in `vita3k/packages/src/pkg.cpp`, pkg2zip-derived) and **PFS inside**, decrypted by a
  vendored batch copy of psvpfstools. Direct pkg playback requires rearchitecting psvpfstools into a
  random-access per-sector decryptor — the single highest-risk component (failure mode: silent data
  corruption mid-game).
- **DLC is required day one.** Under pkg-direct that means multiple simultaneous PFS mounts (base +
  each DLC pkg) plus runtime patch/base union. Under the bundle approach all of that collapses into
  import-time file copies performed by the installer code that already ships and works.
- Runtime UX is identical either way: one pretty-named file per game, no install, instant boot,
  single copy on disk, trivially offloadable. The only cost is a one-time automated import per game.
- The mount interface built in Phase 0 is exactly what a future pkg backend would plug into.

**Honest disk-space note:** a STORE zip is roughly the same size as the installed folder (and as the
pkg). The space win is eliminating the *duplicate* copy and making the single copy a movable file —
not compression. Seekable compression is parked (see §8).

## 2. Goals / Non-Goals

**Goals (v1)**

1. Vita3K boots `Persona 4 Golden.zip` directly — no install step, no `ux0/app` copy.
2. One-time "Import" converts pkg(s) (+ zRIFs) → one bundle zip: base game, update merged in, DLC
   included. Optionally deletes the source pkgs after verification.
3. Saves, trophies, and licenses persist on the real filesystem; deleting/moving a bundle never
   touches them.
4. Android: bundles launchable from the in-app library **and** via an exported intent so ES-DE
   Android can launch by file path.
5. Desktop (Linux/Windows) builds boot bundles too — that's the dev/test vehicle.

**Non-Goals (v1)** — explicitly out of scope:

- Direct-from-pkg playback (Phase Next, §8). No psvpfstools changes of any kind.
- Compressed bundles (STORE only; format has a version field for later).
- Themes, PSM, or anything outside game/patch/DLC categories.
- Modifying or removing the existing install/uninstall flows — they stay untouched and working.
- Upstreaming. Assume a long-lived fork; optimize diffs for rebasing (§7).

## 3. Bundle Format v1

```
Game Name.zip                    # zip64 allowed (>4 GiB games exist); STORE (method 0) only
├── vita3k_bundle.json           # {"version":1,"title_id":"PCSE00123","content_id":"...",
│                                #  "category":"gd","has_patch":true,"dlc":["<CONTENTID>", ...]}
├── app/                         # decrypted game tree == contents of ux0/app/<TITLEID>/
│                                # with the update ALREADY merged over the base at import time
├── addcont/<CONTENTID>/...      # one decrypted tree per DLC == ux0/addcont/<TITLEID>/<CONTENTID>/
└── license/<CONTENTID>.rif      # rifs for base (+patch if separate) + every DLC
```

Rules: forward-slash paths, exact case preserved, no compression (import must enforce method 0),
manifest is required and version-checked at mount. Import copies every rif into the real
`ux0/license/<TITLEID>/` (they're ~512 bytes each and persist like saves); the copies inside the zip
exist so a bundle is self-contained if the data folder is ever wiped.

**Why licenses live on the real FS.** Boot reads `ux0/license/<TITLEID>/<CONTENTID>.rif` via
`get_license` (`app/src/app.cpp:257`, `packages/src/license.cpp:99-104`); if the rif is absent boot
logs a warning and continues — there is **no app-dir raw-`fs::` license fallback in the boot path.**
**[CORRECTED]** The earlier brief claimed a "first-boot fallback that copies `sce_sys/package/work.bin`
out of the app dir" at `interface.cpp:95-98`. That is wrong: `interface.cpp:94-98` lives inside
`is_nonpdrm()`, which is part of the NoNpDrm **install/decrypt** flow and runs only during package
installation — never at boot. The design conclusion is unchanged: because the importer pre-places
every rif into `ux0/license/<TITLEID>/`, boot's `get_license` finds them on the real FS and the mount
never has to serve a license — zero mount complexity for licensing.

## 4. Verified Code Map (where everything hooks)

**The IO funnel is narrow.** Outside the GUI, host paths are built by
`vita3k/io/src/io.cpp` + `vita3k/io/src/device.cpp` (`construct_emulated_path`), with module loads
routed via `module_parent`'s `translate_path` and FMV via `SceAvPlayer`.

- **Full op surface** to route: `io/include/io/functions.h` — `open_file`, `read_file`,
  `write_file`, `truncate_file`, `seek_file`, `tell_file`, `stat_file`, `stat_file_by_fd`,
  `close_file`, `remove_file`, `rename`, `open_dir`, `read_dir`, `create_dir`, `close_dir`,
  `remove_dir`. For paths under a mounted root: reads/stats/dir-listing served from the bundle;
  all mutating ops return `SCE_ERROR_ERRNO_EROFS`.
- **Per-boot device mapping**: `init_device_paths` (`io/src/io.cpp:159`) sets
  `device_paths.app0 = "app/" + io.app_path` and `device_paths.addcont0 = "addcont/" + io.addcont`;
  called from `interface.cpp:461` (`load_app_impl`). `app0:` and `addcont0:` are the two roots the
  mount virtualizes. For games `io.app_path == io.addcont == title_id`
  (`apps_list.cpp:349`/`330`, set in `set_app_info` `apps_list.cpp:580-584`).
  `translate_path` (`io.cpp:222`) collapses `app0:`/`addcont0:` into the `ux0` device; `open_file`
  keeps the original device in `device_for_icase` (`io.cpp:314`) — the clean routing seam.
  Note `find_case_isens_path` slices `substr(0,18)` for addcont (`io.cpp:187`) — replicate its
  semantics, don't "fix" it.

**Verified raw-`fs::` straggler table (host-path touches that bypass emulated IO — route through the
bundle).** Audited across the boot sequence + modules; all P0:

| Site | What it does | Fix |
|---|---|---|
| `io/src/io.cpp:72` `vfs::read_app_file` | param.sfo + loader whole-file reads | route through bundle pread |
| `module_parent` `translate_path` (module loads) | loads `app0:...` modules | served by the mount once app0 routed |
| `interface.cpp:500-509` (`add_preload_module`) **[NEW]** | raw `fs::exists` on `ux0/app/<app_path>/sce_module/*.suprx` to decide whether to preload app-supplied `libc`/`libfios2`/… ; when it passes the *load* goes through `app0:sce_module/...` (already served) | route only the **existence probe** to `bundle.exists()`. Silent-compat-bug factory if missed. |
| `app/src/app.cpp:277-279` (`prepare_game_launch_overlay`) **[NEW]** | stores a **host path string** `renderer.precompile_bg_path = <ux0/app/.../sce_sys/pic0.png>`, consumed later by the renderer thread via `overlay::image_info(path)` (`renderer/src/batch.cpp:190-237`) | **not a one-line exists-swap** — extract `pic0.png` from the bundle to a cache file and point the string there (or teach `image_info` to take bytes) |
| `modules/SceAppUtil/SceAppUtil.cpp:164-166` `is_addcont_exist` | builds `ux0/<addcont0>/<path>` + `fs::exists && !is_empty`; DLC detection | consult the mount, else DLC silently missing |
| `modules/SceAvPlayer/SceAvPlayer.cpp:245-246` | `expand_path` → `fs::exists` → opens host file directly when the game supplies no IO callbacks; FMV | route through the bundle pread path |

**[CORRECTED]** `interface.cpp:94-98` (license/`work.bin`) is **removed** from this list — it is
install-time (`is_nonpdrm`), not a boot straggler (see §3).

- **GUI reads app dirs directly** (`gui-qt/src/apps_list_*.cpp`, `live_area_widget.cpp:254` reads
  `pic0.png` via `QFile`; Android: `AppsListViewModel.kt` / `AppRepository`). **P3** library
  integration reads `param.sfo` / `icon0.png` through the bundle reader and caches metadata per
  bundle. (The Qt live-area `pic0` read is P3, distinct from the P0 renderer path above.)
- **Import reuses existing, battle-tested code**: `install_pkg` (`pkg.cpp:79`) extracts,
  PFS-decrypts, routes by category (`app` / `addcont/<ID>/<CID>` / `patch/<ID>`, `pkg.cpp:218-232`),
  derives the title ID from the pkg header (`content_id.substr(7, 9)`, `pkg.cpp:369`), and writes
  licenses. On Android, `InstallForegroundService` / `InstallServiceController` already accept
  `EXTRA_PATH` / `EXTRA_ZRIF` / `EXTRA_LICENSE_PATH` — add an "import to bundle" operation type to
  this service rather than inventing a new one.
- **Do NOT use `-d`/`--deleted-id`** — it deletes savedata and the shader cache along with the app
  (`main.cpp:135`; `config.cpp:358`). Import cleanup removes only the temporary install directories
  it created.
- **CI**: desktop-build (`ci-linux-clang-appimage`, Qt 6.11.0, clang/cmake/ninja/Vulkan) and
  android-build (`.github/workflows/c-cpp.yml`). The android job produces the APK on every push.

## 5. Implementation Phases

Work desktop-first, flip to Android per phase. Each phase must be green before the next. Keep new
code in new files (`io/src/bundle.cpp`, `bundle_zip.cpp`, `io/include/io/bundle.h`, …); keep diffs
inside existing files minimal.

**P0 — Mount core + directory backend (desktop).** Read-only bundle interface
(`pread`/`stat`/`list_dir`/`exists`) with a *directory* backend. Add bundle-backed fd-table entries,
route the §4 op surface, wire `app0:`/`addcont0:` roots, add a dev-only `--bundle <path>` boot flag,
route `vfs::read_app_file`, `module_parent`, AvPlayer, `is_addcont_exist`, the sce_module preload
probe, and pic0.
- [ ] A game folder copied outside `ux0` boots via `--bundle`; saves write normally
- [ ] Write/rename/remove into `app0:` returns EROFS and the game still runs
- [ ] An FMV-heavy title plays video (AvPlayer routed)
- [ ] A DLC title detects and loads its DLC from the bundle's `addcont/` (rifs hand-placed in
      `ux0/license/<TITLEID>/` until the importer exists)

**P1 — Zip backend.** STORE-zip reader: parse central directory once → entry map → `pread` = offset
read on a single host fd; zip64; enforce method 0 + manifest version at mount. Reuse the
case-insensitive fallback pattern from `io.cpp`.
- [ ] Same acceptance as P0 but from a `.zip`; plus a >4 GiB game boots (zip64)
- [ ] Integrity harness: every file read via emulated IO hash-matches the pre-zip originals

**P2 — Import pipeline.** Desktop CLI + GUI action and Android service op: base pkg + zRIF, optional
update pkg, optional DLC pkgs + zRIFs → temp-install via existing `install_pkg` → **merge update over
base is automatic** (see §9/Q1: `copy_path` overwrites into `ux0/app/<TITLEID>` at install) → write
bundle zip → copy rifs to `ux0/license/<TITLEID>/` → verify (reopen zip, spot-read + manifest) →
delete temp dirs; optionally delete source pkgs only after verification passes.
- [ ] Import of base+update+DLC produces one zip; game boots patched with DLC visible
- [ ] Re-import is idempotent; cancel/failure leaves no temp litter and never touches saves
- [ ] Progress UI on Android via the existing foreground-service pattern

**P3 — Library + external launch.** Bundle library folder in settings; desktop Qt list and Android
`AppRepository` show bundle games (title/icon read through the bundle reader, cached). Android:
exported activity/intent accepting a bundle file path. Document the ES-DE Android custom system entry.
- [ ] Tapping a bundle in-library boots it; ES-DE Android launches a named zip end-to-end

**P4 — Polish.** Corrupt-zip and wrong-version error UX; "remove bundle" that demonstrably never
touches `ux0/user` (saves) or licenses; README for the fork.

## 6. Open Questions — status

- **Q1 (was blocking P2): RESOLVED** — see §9. In-place overwrite at install time; importer merges
  base+update for free.
- **Q2 (was blocking P0): RESOLVED** — see §4 straggler table + §9.
- **Q3 (was blocking P0): RESOLVED** — see §9 (fd table + addcont trigger).
- **Q4 (non-blocking):** `StorageAccess.kt` — confirm the native side receives plain paths (not
  per-read SAF), so zip preads are raw-file speed on Android. (P0-Android.)
- **Q5 (non-blocking until P3):** ES-DE Android's current Vita3K launch mechanism.
- **Q6 (non-blocking):** Trophy `.trp` access path — confirm it flows through emulated IO (then the
  mount serves it for free). (P4.)

## 7. Constraints & Fork Hygiene

- Simplest thing that works; do not touch code unrelated to the task; flag uncertainty and stop
  rather than guess; no speculative abstractions beyond the bundle interface defined here.
- **Rebase strategy:** new functionality in new files; existing-file diffs limited to routing hooks.
  Expect upstream churn in `io.cpp` and the Android UI; keep hooks few and obvious.
- **License:** Vita3K is GPLv2 — the fork and any distributed APKs must remain GPL-compliant.
- **Data-safety invariants (test them):** nothing under `ux0/user` (saves) or `ux0/license` is ever
  written or deleted by mount, import cleanup, or bundle removal.

## 8. Parked / Phase Next

- **Pkg-direct backend:** pkg AES-CTR reader + psvpfstools refactored into a seekable per-sector PFS
  reader + N simultaneous mounts with patch union, all behind the same bundle interface.
- **Seekable compression** (zstd-seekable or chunked-deflate) — bump `vita3k_bundle.json` version.
- **NoNpDrm-dump import** (`decrypt_install_nonpdrm`, `pkg.cpp:59`) as an alternate import source.

## 9. P0 Kickoff — Verified Findings (2026-07-05, commit `24a401f`)

**Q1 — app vs patch → in-place overwrite at *install* time.** There is no runtime `ux0/patch`
consumer. `copy_path` (`io.cpp:785-797`) copies the `gp` patch staging dir's contents *into*
`ux0/app/<TITLEID>` (`copy_directory_contents` defaults to `overwrite_existing`, `util/fs.h:83` — a
true union: patch files overwrite, untouched base files persist), then `fs::remove_all`s the staging
dir. `install_pkg` funnels through the same `copy_path` (`pkg.cpp:347`). The only other `ux0/patch`
mention (`apps_list.cpp:496`) is delete-app housekeeping, not a data path. → Importer: temp-install
base, then temp-install update into the same `ux0`; the merged `ux0/app/<TITLEID>` is the bundle
`app/`. No runtime union, no patch dir in the bundle.

**Q2 — boot-sequence raw-`fs::` audit.** See the §4 straggler table (6 route points; 2 newly found:
sce_module preload probe `interface.cpp:500-509`, pic0 `app.cpp:277-279`). The license check
`interface.cpp:94-98` is install-time, not a boot straggler. No additional app-dir existence gate
exists in the `-r`/auto-boot path (`main.cpp:195-253`) — the boot gate is the in-memory apps-list
lookup in `set_app_info`, which `--bundle` populates via a synthetic `AppEntry`. **Straggler #7
audit: none found.**

**Q3 — fd table + addcont trigger.** `IOState` (`io/include/io/state.h:97-131`) holds
`std::map<SceUID,FileStats> std_files` + `std::map<SceUID,DirStats> dir_entries` keyed off one
monotonic `io.next_fd`. `FileStats`/`DirStats` are concrete value types (not polymorphic); IO methods
in `io/src/state_functions.cpp:41-110`. `FileStats::read` guards `if (!wrapped_file) return -1`, so a
bundle-backed `FileStats` has a null host stream and branches to `bundle->pread`. **Design decision:**
augment `FileStats`/`DirStats` with an optional bundle backend + a second constructor (minimal
op-function diffs) rather than a parallel fd map or a polymorphic refactor. `io.app_path`/`io.addcont`
are set in `set_app_info` (`apps_list.cpp:580-584`), cleared in `io_deinit` (`io.cpp:144`).

**Environment note.** Baseline desktop build is blocked on the dev box (no clang/cmake/ninja/
pkg-config/Vulkan/Qt6; no passwordless sudo). Proceeding code-first per owner decision; compile/run
verification deferred to a provisioned toolchain or CI.
