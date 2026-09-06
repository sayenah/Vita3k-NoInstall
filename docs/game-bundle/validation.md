# Automated library validation

This branch adds automated validation for large Vita3K-NoInstall ROM libraries. The authoritative
result combines **content validation** (base/update/DLC assembly) with **runtime validation** (the game
actually boots and continues rendering).

## What PASS means

A title receives PASS only when all of the following are true:

1. `mount_pkg_for_play` successfully prepares the game using the normal NoInstall path.
2. The independent validation inventory can completely inspect the configured content explicitly
   associated with this TITLEID.
3. If an update is available, the highest expected `app_version` is the version visible in the fully
   prepared temporary `app/` tree.
4. Every DLC content ID expected from the configured DLC folder is present in the mounted temporary
   `addcont/` tree. Extra DLC carried inside the game archive is allowed.
5. Vita3K reaches the normal game boot path.
6. The emulator produces rendered frames.
7. Frame activity is still current after the configured validation runtime; one frame followed by a
   hang is not a PASS.

A boot timeout prevents a broken title from blocking an unattended run indefinitely.

## Independent expected-content inventory

The validator deliberately does not ask the launch resolver to report what it *intended* to load and
then trust that same answer. `packages/validation.*` independently inventories the configured side
folders and compares those expectations with the game tree produced by the normal launch path.

For a TITLEID it mirrors these existing NoInstall layouts:

- Updates: `<TITLEID>/`, `<TITLEID>.zip`, `<TITLEID>.7z`, or loose matching `.pkg` files. Only update
  category `gp` is expected when the package SFO is readable; the highest `app_version` is selected.
- DLC: `<TITLEID>/`, `<TITLEID>.zip`, `<TITLEID>.7z`, loose matching `.pkg` files, and already-decrypted
  `addcont` trees. Package content IDs and decrypted addcont directory names become the expected DLC
  set.
- Licenses: `<TITLEID>/*.rif` and `license.zip` are inventoried for reporting. External RIF content IDs
  are not independently fatal because self-contained NoNpDrm content can supply `work.bin` instead.

If a source explicitly named for the title (for example `PCSE00001.zip`) cannot be inspected, the
inventory is marked incomplete and the title cannot receive PASS. Unreadable unrelated loose packages
are warnings rather than automatically failing every title.

## JSON result

Each game result records at least:

- source path and TITLEID;
- overall status and reason;
- content-validation status and inventory completeness;
- effective and expected update versions, plus whether they match;
- expected, mounted, and missing DLC content IDs;
- configured external RIF content IDs and RIF count visible to Vita3K for the title;
- inventory warnings;
- whether Vita execution started;
- whether rendered frames were observed;
- whether the stable-rendering window completed;
- runtime and timeout settings.

Typical failure reasons include `preparation_failed`, `content_inventory_incomplete`,
`expected_update_not_effective`, `expected_dlc_missing`, `boot_did_not_start_before_timeout`,
`boot_started_but_no_rendered_frames_before_timeout`, and
`rendering_stalled_before_validation_completed`.

## Data safety

Validation boots do not intentionally use the player's normal Vita user. Vita3K creates a temporary
`Vita3K Validator` user, activates it for the validation run, then deletes that user and restores the
original active user after the game stops. This isolates validation-created saves, trophies, and play
history from the normal profile during a clean validation exit.

Game files, updates, and DLC remain subject to the existing read-only NoInstall mount rules.
License-folder behavior is unchanged: matching RIFs may be copied into the global `ux0/license` tree
just as they are during a normal NoInstall launch. Shader/cache warming is also intentionally not
isolated because those caches are disposable emulator data and can make later runs faster.

The outer library runner starts a **fresh Vita3K process per game**. This prevents one emulator crash
from contaminating later titles and makes per-game results independently recoverable. A future cleanup
pass should remove stale validator users left behind by a process that is forcibly killed before the
normal cleanup path runs.

## Single-game invocation

Until the public CLI surface is finalized, validation mode is enabled internally by environment
variables and driven by `tools/validate-game.ps1`:

```powershell
.\tools\validate-game.ps1 `
  -Vita3KExe "S:\Downloads\Vita3K-NoInstall-windows-x64\bin\Vita3K.exe" `
  -Game "S:\ROMs\Sony - Playstation Vita\Base Set\Example Game.zip" `
  -RuntimeSeconds 10 `
  -BootTimeoutSeconds 60
```

The wrapper sets `VITA3K_VALIDATE_GAME`, `VITA3K_VALIDATE_RUNTIME`,
`VITA3K_VALIDATE_BOOT_TIMEOUT`, and `VITA3K_VALIDATE_RESULT`, then launches the existing
`Vita3K.exe --play-pkg <game>` entry point.

## Library runner

`tools/validate-library.ps1` is the crash-resilient outer orchestrator. It scans one or more ROM roots
recursively for `.zip`, `.7z`, and `.pkg` games and launches each game in a new Vita3K process.

Example:

```powershell
.\tools\validate-library.ps1 `
  -Vita3KExe "S:\Downloads\Vita3K-NoInstall-windows-x64\bin\Vita3K.exe" `
  -RomsFolder "S:\ROMs\Sony - Playstation Vita\Base Set" `
  -RuntimeSeconds 10 `
  -BootTimeoutSeconds 60
```

The runner writes:

- one durable JSON result per game under `library-validation/games/`;
- `Vita3K-Library-Validation.csv`, rebuilt after every completed title;
- `Vita3K-Library-Validation-Summary.json` at the end.

Running the same command again resumes from completed per-game JSON files. Use `-Fresh` to discard the
previous output and validate everything again. `-HardProcessTimeoutSeconds` can enforce an outer
process-level kill for a Vita3K instance that becomes stuck outside the normal in-emulator timeout.

The CSV carries the content and runtime fields needed to filter failures: effective/expected update,
expected/mounted/missing DLC IDs, license counts, inventory warnings, boot/render status, and process
exit code.

Sequential operation is intentional because the current NoInstall play path owns one shared
`<cache>/pkgplay` temporary tree.

## Before large-library use

Do not start with the entire collection. First prove the build against a small representative set:

- a game with no update or DLC;
- a game with an update;
- a game with several DLC packages;
- ideally one intentionally broken/missing-content case to confirm that FAIL classification is useful.

After those results look correct, boot/runtime thresholds can be tuned before a thousand-title run.
