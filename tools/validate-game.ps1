param(
    [Parameter(Mandatory = $true)]
    [string]$Game,

    [string]$Vita3KExe = "S:\Downloads\Vita3K-NoInstall-windows-x64\bin\Vita3K.exe",

    [ValidateRange(1, 600)]
    [int]$RuntimeSeconds = 10,

    [ValidateRange(5, 1800)]
    [int]$BootTimeoutSeconds = 60,

    [string]$ResultPath = ""
)

$ErrorActionPreference = "Stop"

$Vita3KExe = [System.IO.Path]::GetFullPath($Vita3KExe)
$Game = [System.IO.Path]::GetFullPath($Game)

if (-not (Test-Path -LiteralPath $Vita3KExe -PathType Leaf)) {
    throw "Vita3K.exe not found: $Vita3KExe"
}
if (-not (Test-Path -LiteralPath $Game -PathType Leaf)) {
    throw "Game file not found: $Game"
}

if ([string]::IsNullOrWhiteSpace($ResultPath)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $ResultPath = Join-Path (Split-Path -Parent $Vita3KExe) "validation-$stamp.json"
}
$ResultPath = [System.IO.Path]::GetFullPath($ResultPath)

$oldValidate = $env:VITA3K_VALIDATE_GAME
$oldRuntime = $env:VITA3K_VALIDATE_RUNTIME
$oldTimeout = $env:VITA3K_VALIDATE_BOOT_TIMEOUT
$oldResult = $env:VITA3K_VALIDATE_RESULT

try {
    $env:VITA3K_VALIDATE_GAME = "1"
    $env:VITA3K_VALIDATE_RUNTIME = [string]$RuntimeSeconds
    $env:VITA3K_VALIDATE_BOOT_TIMEOUT = [string]$BootTimeoutSeconds
    $env:VITA3K_VALIDATE_RESULT = $ResultPath

    Write-Host "Validating: $Game"
    Write-Host "Runtime target: $RuntimeSeconds seconds after first rendered frame"
    Write-Host "Boot timeout: $BootTimeoutSeconds seconds"
    Write-Host "Result: $ResultPath"
    Write-Host ""

    & $Vita3KExe --play-pkg $Game
    $exitCode = $LASTEXITCODE
}
finally {
    $env:VITA3K_VALIDATE_GAME = $oldValidate
    $env:VITA3K_VALIDATE_RUNTIME = $oldRuntime
    $env:VITA3K_VALIDATE_BOOT_TIMEOUT = $oldTimeout
    $env:VITA3K_VALIDATE_RESULT = $oldResult
}

Write-Host ""
if (Test-Path -LiteralPath $ResultPath -PathType Leaf) {
    $result = Get-Content -LiteralPath $ResultPath -Raw | ConvertFrom-Json
    $result | Format-List
} else {
    Write-Warning "Vita3K exited without producing a validation result file."
}

if ($exitCode -eq 0) {
    Write-Host "Validation process returned PASS (exit 0)."
} else {
    Write-Warning "Validation process returned exit code $exitCode."
}

exit $exitCode
