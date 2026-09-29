param(
    [Parameter(Mandatory=$true)][string]$Pio,
    [Parameter(Mandatory=$true)][string]$Work,
    [Parameter(Mandatory=$true)][string]$Port
)

$ErrorActionPreference = "Continue"
$log = Join-Path $Work "r2f_upload.log"
if (Test-Path $log) { Remove-Item $log -Force }

Write-Host "============================================================"
Write-Host "FLASH $Port"
Write-Host "============================================================"

$out = & $Pio run -d $Work -e m5stack-cardputer -t upload --upload-port $Port 2>&1
$code = $LASTEXITCODE
$out | Tee-Object -FilePath $log | ForEach-Object { Write-Host $_ }

if ($code -eq 0) {
    Write-Host "CCCP R2F FLASH PASS"
    exit 0
}

$text = ($out | Out-String)

# R1W already proved that native USB can fail only after the image is fully written.
$writeCompleted =
    (($text -match "Writing at .*100 ?%") -or ($text -match "\(100 ?%\)")) -and
    ($text -match "Wrote [0-9]+ bytes")

$postResetUsbError =
    $text -match "Cannot configure port|PermissionError\(13|serial exception|device.*command|Access is denied"

if ($writeCompleted -and $postResetUsbError) {
    Write-Host ""
    Write-Host "CCCP R2F FLASH WRITE COMPLETE"
    Write-Host "Ignoring known native-USB post-write/reset error."
    exit 0
}

Write-Host ""
Write-Host "CCCP R2F FLASH FAILED BEFORE CONFIRMED COMPLETE WRITE"
if ($code -eq 0) { exit 1 }
exit $code
