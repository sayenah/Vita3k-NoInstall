param(
    [Parameter(Mandatory = $true)]
    [string[]]$RomsFolder,

    [string]$Vita3KExe = "S:\Downloads\Vita3K-NoInstall-windows-x64\bin\Vita3K.exe",

    [ValidateRange(1, 600)]
    [int]$RuntimeSeconds = 10,

    [ValidateRange(5, 1800)]
    [int]$BootTimeoutSeconds = 60,

    [string]$OutputDirectory = "",

    [switch]$Fresh,

    [ValidateRange(0, 86400)]
    [int]$HardProcessTimeoutSeconds = 0
)

$ErrorActionPreference = "Stop"

function Get-PathHash {
    param([Parameter(Mandatory = $true)][string]$Path)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Path.ToLowerInvariant())
        $hash = $sha.ComputeHash($bytes)
        return ([System.BitConverter]::ToString($hash) -replace '-', '').Substring(0, 16)
    }
    finally {
        $sha.Dispose()
    }
}

function Get-SafeStem {
    param([Parameter(Mandatory = $true)][string]$Name)

    $invalid = [System.IO.Path]::GetInvalidFileNameChars()
    $builder = [System.Text.StringBuilder]::new()
    foreach ($c in $Name.ToCharArray()) {
        if ($invalid -contains $c) {
            [void]$builder.Append('_')
        }
        else {
            [void]$builder.Append($c)
        }
    }
    $value = $builder.ToString().Trim()
    if ($value.Length -gt 80) {
        $value = $value.Substring(0, 80)
    }
    if ([string]::IsNullOrWhiteSpace($value)) {
        return "game"
    }
    return $value
}

function New-FailureRow {
    param(
        [Parameter(Mandatory = $true)][string]$GamePath,
        [Parameter(Mandatory = $true)][string]$Reason,
        [Parameter(Mandatory = $true)][int]$ExitCode
    )

    return [pscustomobject]@{
        source_path                      = $GamePath
        title_id                         = ""
        status                           = "fail"
        reason                           = $Reason
        content_validation_pass          = $false
        inventory_complete               = $false
        effective_app_version            = ""
        expected_update_version          = ""
        update_version_matches           = $false
        expected_dlc                     = 0
        mounted_dlc                      = 0
        expected_dlc_content_ids         = ""
        mounted_dlc_content_ids          = ""
        missing_dlc_content_ids          = ""
        external_license_content_ids     = ""
        configured_external_license_rifs = 0
        license_rifs                     = 0
        inventory_warnings               = ""
        boot_started                     = $false
        frames_observed                  = $false
        stable_runtime_complete          = $false
        requested_runtime_seconds        = $RuntimeSeconds
        boot_timeout_seconds             = $BootTimeoutSeconds
        observed_runtime_ms              = 0
        process_exit_code                = $ExitCode
    }
}

function Read-ValidationResult {
    param(
        [Parameter(Mandatory = $true)][string]$ResultPath,
        [Parameter(Mandatory = $true)][string]$GamePath,
        [Parameter(Mandatory = $true)][int]$ExitCode
    )

    if (-not (Test-Path -LiteralPath $ResultPath -PathType Leaf)) {
        return New-FailureRow -GamePath $GamePath -Reason "process_exited_without_result" -ExitCode $ExitCode
    }

    try {
        $data = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json
    }
    catch {
        return New-FailureRow -GamePath $GamePath -Reason "invalid_result_json: $($_.Exception.Message)" -ExitCode $ExitCode
    }

    $updateVersionMatches = if ($null -ne $data.PSObject.Properties['update_version_matches']) {
        [bool]$data.update_version_matches
    }
    else {
        # Preserve resumability for JSON written by validator builds before this field was pluralized.
        [bool]$data.update_version_match
    }

    return [pscustomobject]@{
        source_path                      = [string]$data.source_path
        title_id                         = [string]$data.title_id
        status                           = [string]$data.status
        reason                           = [string]$data.reason
        content_validation_pass          = [bool]$data.content_validation_pass
        inventory_complete               = [bool]$data.inventory_complete
        effective_app_version            = [string]$data.effective_app_version
        expected_update_version          = [string]$data.expected_update_version
        update_version_matches           = $updateVersionMatches
        expected_dlc                     = [int]$data.expected_dlc
        mounted_dlc                      = [int]$data.mounted_dlc
        expected_dlc_content_ids         = (@($data.expected_dlc_content_ids) -join ';')
        mounted_dlc_content_ids          = (@($data.mounted_dlc_content_ids) -join ';')
        missing_dlc_content_ids          = (@($data.missing_dlc_content_ids) -join ';')
        external_license_content_ids     = (@($data.external_license_content_ids) -join ';')
        configured_external_license_rifs = [int]$data.configured_external_license_rifs
        license_rifs                     = [int]$data.license_rifs
        inventory_warnings               = (@($data.inventory_warnings) -join ' | ')
        boot_started                     = [bool]$data.boot_started
        frames_observed                  = [bool]$data.frames_observed
        stable_runtime_complete          = [bool]$data.stable_runtime_complete
        requested_runtime_seconds        = [int]$data.requested_runtime_seconds
        boot_timeout_seconds             = [int]$data.boot_timeout_seconds
        observed_runtime_ms              = [int]$data.observed_runtime_ms
        process_exit_code                = $ExitCode
    }
}

$Vita3KExe = [System.IO.Path]::GetFullPath($Vita3KExe)
if (-not (Test-Path -LiteralPath $Vita3KExe -PathType Leaf)) {
    throw "Vita3K.exe not found: $Vita3KExe"
}

$resolvedRoots = @()
foreach ($root in $RomsFolder) {
    $full = [System.IO.Path]::GetFullPath($root)
    if (-not (Test-Path -LiteralPath $full -PathType Container)) {
        throw "ROMs folder not found: $full"
    }
    if ($resolvedRoots -notcontains $full) {
        $resolvedRoots += $full
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path (Split-Path -Parent $Vita3KExe) "library-validation"
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
$perGameDirectory = Join-Path $OutputDirectory "games"
$csvPath = Join-Path $OutputDirectory "Vita3K-Library-Validation.csv"
$summaryPath = Join-Path $OutputDirectory "Vita3K-Library-Validation-Summary.json"

if ($Fresh -and (Test-Path -LiteralPath $OutputDirectory)) {
    Remove-Item -LiteralPath $OutputDirectory -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $perGameDirectory | Out-Null

$games = foreach ($root in $resolvedRoots) {
    Get-ChildItem -LiteralPath $root -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension.ToLowerInvariant() -in @('.zip', '.7z', '.pkg') }
}
$games = @($games | Sort-Object FullName -Unique)

if ($games.Count -eq 0) {
    throw "No .zip, .7z, or .pkg games were found under the supplied ROMs folders."
}

Write-Host "============================================================"
Write-Host "Vita3K-NoInstall Library Validation"
Write-Host "============================================================"
Write-Host "Games discovered: $($games.Count)"
Write-Host "Runtime target:   $RuntimeSeconds seconds after rendered frames begin"
Write-Host "Boot timeout:     $BootTimeoutSeconds seconds"
if ($HardProcessTimeoutSeconds -gt 0) {
    Write-Host "Hard timeout:     $HardProcessTimeoutSeconds seconds per Vita3K process"
}
else {
    Write-Host "Hard timeout:     disabled"
}
Write-Host "Output:           $OutputDirectory"
Write-Host ""

$results = [System.Collections.Generic.List[object]]::new()
$passCount = 0
$failCount = 0
$resumedCount = 0

for ($index = 0; $index -lt $games.Count; $index++) {
    $game = $games[$index]
    $hash = Get-PathHash -Path $game.FullName
    $stem = Get-SafeStem -Name $game.BaseName
    $resultPath = Join-Path $perGameDirectory ("{0}__{1}.json" -f $stem, $hash)

    Write-Host "------------------------------------------------------------"
    Write-Host ("[{0} / {1}] {2}" -f ($index + 1), $games.Count, $game.FullName)

    if (-not $Fresh -and (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
        $existingExitCode = 2
        try {
            $existingRaw = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
            if ($existingRaw.status -eq 'pass') {
                $existingExitCode = 0
            }
            $existing = Read-ValidationResult -ResultPath $resultPath -GamePath $game.FullName -ExitCode $existingExitCode
            $results.Add($existing)
            if ($existing.status -eq 'pass') { $passCount++ } else { $failCount++ }
            $resumedCount++
            Write-Host "RESUME: existing result = $($existing.status.ToUpperInvariant()) ($($existing.reason))"
            $results | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8
            continue
        }
        catch {
            Write-Warning "Existing result is unreadable; re-validating this game."
            Remove-Item -LiteralPath $resultPath -Force -ErrorAction SilentlyContinue
        }
    }

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Vita3KExe
    $psi.UseShellExecute = $false
    $psi.WorkingDirectory = Split-Path -Parent $Vita3KExe
    $psi.ArgumentList.Add('--play-pkg')
    $psi.ArgumentList.Add($game.FullName)
    $psi.Environment['VITA3K_VALIDATE_GAME'] = '1'
    $psi.Environment['VITA3K_VALIDATE_RUNTIME'] = [string]$RuntimeSeconds
    $psi.Environment['VITA3K_VALIDATE_BOOT_TIMEOUT'] = [string]$BootTimeoutSeconds
    $psi.Environment['VITA3K_VALIDATE_RESULT'] = $resultPath

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $psi

    try {
        if (-not $process.Start()) {
            throw "Failed to start Vita3K.exe"
        }

        $timedOut = $false
        if ($HardProcessTimeoutSeconds -gt 0) {
            if (-not $process.WaitForExit($HardProcessTimeoutSeconds * 1000)) {
                $timedOut = $true
                Write-Warning "Hard process timeout reached; terminating Vita3K."
                $process.Kill($true)
                $process.WaitForExit()
            }
        }
        else {
            $process.WaitForExit()
        }

        $exitCode = $process.ExitCode
        if ($timedOut -and -not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
            $timeoutRow = New-FailureRow -GamePath $game.FullName -Reason "hard_process_timeout" -ExitCode $exitCode
            $timeoutRow | Select-Object -ExcludeProperty process_exit_code |
                ConvertTo-Json | Set-Content -LiteralPath $resultPath -Encoding UTF8
        }

        $row = Read-ValidationResult -ResultPath $resultPath -GamePath $game.FullName -ExitCode $exitCode
        if (-not (Test-Path -LiteralPath $resultPath -PathType Leaf)) {
            $row | Select-Object -ExcludeProperty process_exit_code |
                ConvertTo-Json | Set-Content -LiteralPath $resultPath -Encoding UTF8
        }
    }
    catch {
        $row = New-FailureRow -GamePath $game.FullName -Reason "launcher_exception: $($_.Exception.Message)" -ExitCode -1
        $row | Select-Object -ExcludeProperty process_exit_code |
            ConvertTo-Json | Set-Content -LiteralPath $resultPath -Encoding UTF8
    }
    finally {
        if ($process) {
            $process.Dispose()
        }
    }

    $results.Add($row)
    if ($row.status -eq 'pass') {
        $passCount++
        Write-Host "PASS: $($row.title_id) | app $($row.effective_app_version) | DLC $($row.mounted_dlc)/$($row.expected_dlc) | RIFs $($row.license_rifs)"
    }
    else {
        $failCount++
        Write-Warning "FAIL: $($row.title_id) | $($row.reason)"
        if (-not [string]::IsNullOrWhiteSpace($row.missing_dlc_content_ids)) {
            Write-Warning "Missing DLC: $($row.missing_dlc_content_ids)"
        }
    }

    # Rebuild the CSV after every title. If the host or emulator crashes later, all completed results
    # remain durable and the next run can resume from the per-game JSON files.
    $results | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8
}

$summary = [ordered]@{
    generated_at             = (Get-Date).ToString('o')
    games_discovered         = $games.Count
    passed                   = $passCount
    failed                   = $failCount
    resumed_existing_results = $resumedCount
    runtime_seconds          = $RuntimeSeconds
    boot_timeout_seconds     = $BootTimeoutSeconds
    hard_process_timeout     = $HardProcessTimeoutSeconds
    roms_folders             = $resolvedRoots
    csv                      = $csvPath
}
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host ""
Write-Host "============================================================"
Write-Host "Validation complete"
Write-Host "============================================================"
Write-Host "Games:   $($games.Count)"
Write-Host "PASS:    $passCount"
Write-Host "FAIL:    $failCount"
Write-Host "Resumed: $resumedCount"
Write-Host "CSV:     $csvPath"
Write-Host "Summary: $summaryPath"

if ($failCount -gt 0) {
    exit 2
}
exit 0
