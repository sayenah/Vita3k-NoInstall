# Vita3K NoInstall library validator handoff

This document is the handoff for continuing validator development and real-game testing on the
Windows computer.

## Repository state

- Repository: <https://github.com/sayenah/Vita3k-NoInstall>
- Working branch: `feature/library-validator`
- Base branch: `feature/game-bundle`
- Pull request: [#5 - Library validator: automated boot + runtime validation](https://github.com/sayenah/Vita3k-NoInstall/pull/5)
- Validated implementation commit: `e5df4dd35ead0be60985a02b34d830d5d6a8f65e`

Do not merge PR #5 until real game, update, and DLC validation has been demonstrated.

## Completed work

The original Windows compiler failure is fixed. The complete `Config` definition is in
`vita3k/config/include/config/state.h`, and `vita3k/packages/src/validation.cpp` now includes that
header directly. The apparent `collect_pkg_candidates()` and `inventory_container()` signature
errors were cascading MSVC diagnostics caused by the incomplete `Config` type.

Relevant commits:

- `d4ecf29a` — include the complete Config type in the implementation.
- `c53a446e` — mirror target-specific side-content discovery.
- `2b87c435` — tighten runtime and result guarantees.
- `e5df4dd3` — compare packaged DLC using the addcont IDs actually mounted by Vita3K.

The review also corrected the following behavior:

1. Title-specific update and DLC directories/archives use the same match-all behavior as the
   launcher; loose PKGs remain matched by embedded TITLEID.
2. Inventory scratch directories are checked for cleanup and creation errors.
3. Full PSN DLC content IDs are normalized to the suffix `install_pkg` uses for the mounted addcont
   directory. The full ID remains available in the inventory record.
4. Runtime activity requires the internal frame counter to advance. A counter that remains nonzero
   no longer proves sustained rendering.
5. Rendering that resumes after a stall restarts the stable-runtime window.
6. Result JSON exposes `update_version_matches`, expected/mounted/missing DLC IDs, and
   `external_license_content_ids`.
7. Successful validation uses the reason `content_verified_booted_and_rendered`.
8. Preparation failures in validation mode return exit code 2.
9. The library runner persists a failure if Vita3K exits without JSON, rebuilds its CSV when
   resuming, and serializes arrays into readable CSV values.

Important content failure reasons are:

- `content_inventory_incomplete`
- `expected_update_not_effective`
- `expected_dlc_missing`

## Build verification and artifact

GitHub Actions run [34014830695](https://github.com/sayenah/Vita3k-NoInstall/actions/runs/34014830695)
passed on Windows x64, Windows ARM64, Linux x64, Linux ARM64, macOS x64, macOS ARM64, and Android.

- Windows artifact: `vita3k-e5df4dd3-windows-x64`
- Expected `Vita3K.exe` SHA-256:
  `5cbe6f5e738906e4aef119a3f3eeb7aa6bab8b79128acfda6be0fc8ab1c04803`

The artifact contains the new expected-update/DLC inventory logic. Do not use the older
`vita3k-31cbbee4-windows-x64` artifact for content validation.

## Immediate Windows assignment

Perform staged real-game testing. Do not start the 1,000+ title library.

First synchronize the repository:

```powershell
git fetch origin
git switch feature/library-validator
git pull --ff-only origin feature/library-validator
git status --short
```

Download and verify the successful Windows artifact:

```powershell
$Stage = Join-Path $env:TEMP "vita3k-validator-e5df4dd3-$([guid]::NewGuid())"

New-Item -ItemType Directory -Path $Stage | Out-Null

gh run download 34014830695 `
    --repo sayenah/Vita3k-NoInstall `
    --name "vita3k-e5df4dd3-windows-x64" `
    --dir $Stage

Get-FileHash (Join-Path $Stage "bin\Vita3K.exe") -Algorithm SHA256
```

Install it into the separate validator test installation after closing Vita3K:

```powershell
$Install = "S:\Downloads\Vita3K-Validator-Test"

Copy-Item -Path (Join-Path $Stage "bin\*") `
    -Destination (Join-Path $Install "bin") `
    -Recurse -Force

$Vita3K = Join-Path $Install "bin\Vita3K.exe"
Get-FileHash $Vita3K -Algorithm SHA256
```

Do not modify the user's normal Vita3K installation.

## PowerShell validation helper

Use PowerShell 7:

```powershell
$Vita3K = "S:\Downloads\Vita3K-Validator-Test\bin\Vita3K.exe"

function Invoke-VitaValidation {
    param(
        [Parameter(Mandatory)]
        [string]$Game,

        [Parameter(Mandatory)]
        [string]$ResultName,

        [int]$RuntimeSeconds = 10,

        [int]$BootTimeoutSeconds = 60
    )

    $Result = Join-Path (Split-Path $Vita3K -Parent) $ResultName

    $env:VITA3K_VALIDATE_GAME = "1"
    $env:VITA3K_VALIDATE_RUNTIME = [string]$RuntimeSeconds
    $env:VITA3K_VALIDATE_BOOT_TIMEOUT = [string]$BootTimeoutSeconds
    $env:VITA3K_VALIDATE_RESULT = $Result

    Remove-Item $Result -Force -ErrorAction SilentlyContinue

    & $Vita3K --play-pkg $Game
    $ExitCode = $LASTEXITCODE

    "`nExit code: $ExitCode"

    if (Test-Path -LiteralPath $Result) {
        $Raw = Get-Content -LiteralPath $Result -Raw
        $Raw
        return $Raw | ConvertFrom-Json
    }

    Write-Warning "No validation result was created."
    return $null
}
```

## Test 1: known-good simple title

Known-good game:

```text
R:\ROMs\Sony - Playstation Vita\psvita\Base Set (App)\Adventures of Mana (USA).zip
```

Run:

```powershell
$GoodGame = "R:\ROMs\Sony - Playstation Vita\psvita\Base Set (App)\Adventures of Mana (USA).zip"

$GoodResult = Invoke-VitaValidation `
    -Game $GoodGame `
    -ResultName "validation-known-good.json"

$GoodResult | Format-List
```

Required result:

- Process exit code is 0.
- `status = pass`
- `reason = content_verified_booted_and_rendered`
- `content_validation_pass = true`
- `inventory_complete = true`
- `update_version_matches = true`
- `boot_started = true`
- `frames_observed = true`
- `stable_runtime_complete = true`

If this fails, preserve the raw result JSON, Vita3K log, process exit code, exact game path, and
artifact hash. Diagnose those before changing code.

## Test 2: intentional failure

Create a deliberately invalid PKG and validate it:

```powershell
$BrokenGame = Join-Path $env:TEMP "vita3k-intentional-invalid.pkg"
[IO.File]::WriteAllBytes($BrokenGame, [byte[]](0,1,2,3,4,5,6,7))

$BrokenResult = Invoke-VitaValidation `
    -Game $BrokenGame `
    -ResultName "validation-intentional-failure.json"
```

Required result:

- Process exit code is 2.
- `status = fail`
- `content_validation_pass = false`
- `boot_started = false`
- `frames_observed = false`
- `stable_runtime_complete = false`
- The reason begins with `preparation_failed`.

This test must prove that merely launching the process cannot produce PASS.

Return the raw JSON and exit code for Tests 1 and 2 before proceeding.

## Test 3: title with an update

Choose a known title with an update in the configured Updates folder. Verify:

- `expected_update_version` is populated.
- `effective_app_version` is populated.
- `update_version_matches = true`.
- `content_validation_pass = true`.
- Overall status is PASS when runtime validation also succeeds.

The expected version must be the highest lexicographic `app_version`, matching the NoInstall
launcher. If the base game boots but the update is not effective, the result must be FAIL with
`reason = expected_update_not_effective`.

## Test 4: title with DLC

Choose a known title with packaged or decrypted DLC. Verify:

- `expected_dlc > 0`.
- `expected_dlc_content_ids` includes every expected addcont ID.
- `mounted_dlc_content_ids` includes every expected ID.
- `missing_dlc_content_ids` is empty.
- `content_validation_pass = true`.

Package content IDs are intentionally normalized to the mounted addcont suffix. Do not change this
back to comparing full PSN content IDs with directory suffixes. Extra DLC contained inside the base
game archive is allowed.

## Test 5: deliberately hide one DLC

Temporarily move one DLC item outside the configured DLC tree. Use a reversible move and record the
exact source and temporary destination. Validate the game again.

Required result:

- The base game may still boot and render.
- `missing_dlc_content_ids` contains the hidden DLC ID.
- `content_validation_pass = false`.
- `status = fail`.
- `reason = expected_dlc_missing`.
- Process exit code is 2.

Restore the DLC immediately afterward and verify that it is back at its original path. Never delete
DLC or licenses for this test.

## Evidence to preserve

Keep each per-game JSON result. Report at least:

- `source_path`
- `title_id`
- `status`
- `reason`
- `content_validation_pass`
- `inventory_complete`
- `expected_update_version`
- `effective_app_version`
- `update_version_matches`
- `expected_dlc`
- `expected_dlc_content_ids`
- `mounted_dlc_content_ids`
- `missing_dlc_content_ids`
- `external_license_content_ids`
- `inventory_warnings`
- `boot_started`
- `frames_observed`
- `stable_runtime_complete`
- Process exit code

Do not use screenshots or black-screen detection. Runtime validation uses Vita3K's internal
execution state and frame counter.

## Constraints

- Do not redesign the validator architecture.
- Use one Vita3K process per title.
- Do not start the entire library yet.
- Do not merge PR #5.
- Do not weaken content failure conditions to make a test pass.
- A game booting while an expected update or DLC is missing is FAIL.
- Keep further code changes narrow and evidence-driven.
- Inspect JSON and Vita3K logs before patching a failed test.
- Commit and push only verified fixes to `feature/library-validator`.
