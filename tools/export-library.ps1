<#
.SYNOPSIS
    Converts every game in one or more ROMs folders into a single self-contained zip per game.

.DESCRIPTION
    Runs `Vita3K --export-bundle <game> --output <zip>` once per game. Each output zip holds the
    decrypted game with its latest update merged in, every DLC (decrypted) and the game's licenses,
    taken from the Updates/DLCs/License folders configured in that Vita3K installation.

    Source files are never modified or deleted. Outputs mirror the input folder structure under
    -OutputFolder and keep the game's file name (always with a .zip extension). Games whose output
    already exists are skipped, so an interrupted run can simply be started again.

    Vita3K refuses to write a zip unless the update it found is merged in and every expected DLC is
    included and decrypted; those games are reported as failed in export-results.csv.

.EXAMPLE
    pwsh tools/export-library.ps1 -RomsFolder "R:\ROMs\psvita\Base Set (App)" `
        -OutputFolder "R:\ROMs\psvita\Complete" -Vita3KExe "E:\Vita3K\Vita3K.exe"
#>
param(
    [Parameter(Mandatory = $true)]
    [string[]]$RomsFolder,

    [Parameter(Mandatory = $true)]
    [string]$OutputFolder,

    [Parameter(Mandatory = $true)]
    [string]$Vita3KExe,

    # Only convert games whose file name matches one of these wildcards (e.g. "SteamWorld*").
    [string[]]$Filter = @("*"),

    # Vita3K's log file; defaults to portable\vita3k.log (portable installs), else vita3k.log next
    # to the executable.
    [string]$LogPath = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Vita3KExe -PathType Leaf)) {
    throw "Vita3K executable not found: $Vita3KExe"
}
if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $exeDir = Split-Path -Parent $Vita3KExe
    $LogPath = Join-Path $exeDir "portable\vita3k.log"
    if (-not (Test-Path -LiteralPath (Join-Path $exeDir "portable"))) {
        $LogPath = Join-Path $exeDir "vita3k.log"
    }
}

New-Item -ItemType Directory -Path $OutputFolder -Force | Out-Null
$OutputFolder = (Resolve-Path -LiteralPath $OutputFolder).Path
$resultsPath = Join-Path $OutputFolder "export-results.csv"

$games = foreach ($root in $RomsFolder) {
    $rootPath = (Resolve-Path -LiteralPath $root).Path
    Get-ChildItem -LiteralPath $rootPath -Recurse -File |
        Where-Object { $name = $_.Name; $_.Extension -in '.zip', '.7z', '.pkg' -and @($Filter | Where-Object { $name -like $_ }).Count -gt 0 } |
        Where-Object { -not $_.FullName.StartsWith($OutputFolder, [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object {
            $relative = $_.FullName.Substring($rootPath.Length).TrimStart('\', '/')
            [pscustomobject]@{
                Source = $_
                Output = Join-Path $OutputFolder ([IO.Path]::ChangeExtension($relative, ".zip"))
            }
        }
}

$rows = [System.Collections.Generic.List[object]]::new()
$index = 0
foreach ($game in $games) {
    $index++
    $source = $game.Source
    Write-Host ("[{0}/{1}] {2}" -f $index, $games.Count, $source.Name)

    if (Test-Path -LiteralPath $game.Output) {
        $rows.Add([pscustomobject]@{
                source = $source.FullName; output = $game.Output; status = "skipped"; title_id = ""
                app_version = ""; dlc = ""; licenses = ""; source_mb = [int]($source.Length / 1MB)
                output_mb = [int]((Get-Item -LiteralPath $game.Output).Length / 1MB); message = "output already exists"
            })
        continue
    }

    Remove-Item -LiteralPath $LogPath -Force -ErrorAction SilentlyContinue
    $psi = [System.Diagnostics.ProcessStartInfo]::new($Vita3KExe)
    $psi.UseShellExecute = $false
    $psi.ArgumentList.Add('--export-bundle')
    $psi.ArgumentList.Add($source.FullName)
    $psi.ArgumentList.Add('--output')
    $psi.ArgumentList.Add($game.Output)
    $started = Get-Date
    $process = [System.Diagnostics.Process]::Start($psi)
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    $seconds = [int]((Get-Date) - $started).TotalSeconds

    $logLine = ""
    if (Test-Path -LiteralPath $LogPath) {
        $logLine = Select-String -LiteralPath $LogPath -Pattern 'EXPORT (OK|FAILED)' |
            Select-Object -Last 1 | ForEach-Object { $_.Line }
    }

    $row = [pscustomobject]@{
        source = $source.FullName; output = $game.Output; status = "failed"; title_id = ""
        app_version = ""; dlc = ""; licenses = ""; source_mb = [int]($source.Length / 1MB); output_mb = 0
        message = "exit code $exitCode after ${seconds}s"
    }
    if ($exitCode -eq 0 -and (Test-Path -LiteralPath $game.Output) -and
        $logLine -match 'EXPORT OK \[(\w+)\] app_version=(\S*) dlc=\[([^\]]*)\] licenses=(\d+)') {
        $row.status = "ok"
        $row.title_id = $Matches[1]
        $row.app_version = $Matches[2]
        $row.dlc = $Matches[3]
        $row.licenses = $Matches[4]
        $row.output_mb = [int]((Get-Item -LiteralPath $game.Output).Length / 1MB)
        $row.message = "${seconds}s"
    }
    elseif ($exitCode -eq 0 -and (Test-Path -LiteralPath $game.Output)) {
        # Vita3K only exits 0 after writing a complete zip; the details just weren't in the log.
        $row.status = "ok"
        $row.output_mb = [int]((Get-Item -LiteralPath $game.Output).Length / 1MB)
        $row.message = "${seconds}s (no EXPORT line found in $LogPath)"
    }
    elseif ($logLine -match 'EXPORT FAILED for .*?: (.*)$') {
        $row.message = $Matches[1]
    }
    $rows.Add($row)
    $rows | Export-Csv -LiteralPath $resultsPath -Encoding utf8
    Write-Host ("    {0}: {1}" -f $row.status, $row.message)
}

$rows | Export-Csv -LiteralPath $resultsPath -Encoding utf8
$ok = @($rows | Where-Object status -eq 'ok').Count
$failed = @($rows | Where-Object status -eq 'failed').Count
$skipped = @($rows | Where-Object status -eq 'skipped').Count
Write-Host ""
Write-Host "Converted $ok, failed $failed, skipped $skipped. Results: $resultsPath"
if ($failed -gt 0) {
    exit 1
}
