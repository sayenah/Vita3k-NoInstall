# Automated library validation

This branch adds automated validation for large Vita3K-NoInstall ROM libraries.

## Validation levels

### Single-game boot validation (implemented first)

The validator deliberately reuses the normal `--play-pkg` path. A game is not considered a runtime
PASS merely because its archive could be unpacked or its main thread was created. PASS requires:

1. `mount_pkg_for_play` successfully prepares the game using the configured Updates, DLCs and License
   folders.
2. Vita3K reaches the normal `load_and_run()` boot path.
3. The emulator actually produces rendered frames.
4. Rendering remains active for the configured validation runtime.

The validator then stops the title and exits automatically. A boot timeout prevents a broken title
from blocking an unattended run forever.

The current JSON result records:

- source path and title id;
- effective `app_version` from the fully prepared temporary `app/` tree;
- number of mounted DLC content directories under temporary `addcont/`;
- number of `.rif` files visible for the title;
- whether Vita execution started;
- whether rendered frames were observed;
- whether the requested stable-runtime window completed.

This first step proves the boot harness. The next validator layer will instrument the package resolver
itself so the report can distinguish **expected** updates/DLC/licenses from **successfully applied**
ones and identify individual failed content IDs.

## Data safety

Validation boots do not use the player's normal Vita user. Before boot, Vita3K creates a temporary
`Vita3K Validator` user, activates it for the validation run, and after the game is stopped deletes the
entire temporary user tree and restores the originally active user.

This isolates validation-created saves, trophies and play-history from the real profile. Game files,
updates and DLC remain subject to the existing read-only NoInstall mount rules. License-folder behavior
is unchanged: matching RIFs may be copied into the global `ux0/license` tree just as they are during a
normal NoInstall launch.

Shader/cache warming is intentionally not isolated; those caches are disposable emulator data and can
make later validation runs faster.

## Current invocation

Until the CLI surface is finalized, validation mode is enabled by environment variables and driven by
`tools/validate-game.ps1`:

```powershell
.\tools\validate-game.ps1 `
  -Vita3KExe "S:\Downloads\Vita3K-NoInstall-windows-x64\bin\Vita3K.exe" `
  -Game "S:\ROMs\Sony - Playstation Vita\Base Set\Example Game.zip" `
  -RuntimeSeconds 10 `
  -BootTimeoutSeconds 60
```

The wrapper sets:

- `VITA3K_VALIDATE_GAME=1`
- `VITA3K_VALIDATE_RUNTIME=<seconds>`
- `VITA3K_VALIDATE_BOOT_TIMEOUT=<seconds>`
- `VITA3K_VALIDATE_RESULT=<json path>`

and then launches the existing `Vita3K.exe --play-pkg <game>` entry point.

## Planned library mode

Once single-title validation is proven on real libraries, `--validate-library` will iterate the
configured ROM folders sequentially, persist each result immediately for crash/resume safety, and
produce CSV + JSON summaries. Sequential operation is intentional because the current NoInstall play
path owns one shared `<cache>/pkgplay` temporary tree.
