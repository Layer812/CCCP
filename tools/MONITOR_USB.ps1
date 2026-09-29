param(
    [string]$PreferredPort = "COM9",
    [int]$Baud = 115200
)

$ErrorActionPreference = 'Stop'

function Get-SerialCandidates {
    $rows = @()
    try {
        $rows = @(Get-CimInstance Win32_SerialPort | ForEach-Object {
            [pscustomobject]@{
                Port = $_.DeviceID
                Name = $_.Name
                Pnp  = $_.PNPDeviceID
            }
        })
    } catch {
        $rows = @()
    }

    if ($rows.Count -eq 0) {
        $rows = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object | ForEach-Object {
            [pscustomobject]@{ Port = $_; Name = $_; Pnp = '' }
        })
    }
    return $rows
}

# Flash hard-reset can briefly remove/re-enumerate the native USB port.
# Retry discovery for up to about 10 seconds instead of failing immediately.
$ports = @()
for ($attempt = 0; $attempt -lt 40; $attempt++) {
    $ports = @(Get-SerialCandidates)
    if ($ports.Count -gt 0) { break }
    Start-Sleep -Milliseconds 250
}

Write-Host "CCCP R1K serial-port probe:"
if ($ports.Count -eq 0) {
    Write-Host "  no COM ports are visible"
    exit 2
}
foreach ($p in $ports) {
    Write-Host ("  {0}: {1} {2}" -f $p.Port, $p.Name, $p.Pnp)
}

$selected = $null
if ($PreferredPort -and ($PreferredPort.ToUpper() -ne 'AUTO')) {
    $selected = $ports | Where-Object { $_.Port.ToUpper() -eq $PreferredPort.ToUpper() } | Select-Object -First 1
}
if (-not $selected) {
    $selected = $ports | Where-Object {
        ($_.Pnp -match 'VID_303A') -or ($_.Name -match 'Espressif|USB JTAG/serial|USB Serial/JTAG')
    } | Select-Object -First 1
}
if (-not $selected -and $ports.Count -eq 1) {
    $selected = $ports[0]
}
if (-not $selected) {
    Write-Host "ERROR: could not safely choose the Cardputer ADV port."
    Write-Host "Run: R1_MONITOR_COM9.bat COM5"
    exit 3
}

$portName = $selected.Port
Write-Host ""
Write-Host "CCCP R1K MONITOR: $portName @ $Baud"
Write-Host "Monitor is now live. Press Cardputer RESET once to capture BOOT0 onward."
Write-Host "Ctrl-C exits this monitor window."
Write-Host ('-' * 72)

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $portName
$sp.BaudRate = $Baud
$sp.DataBits = 8
$sp.Parity = [System.IO.Ports.Parity]::None
$sp.StopBits = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.ReadTimeout = 100
$sp.WriteTimeout = 500
$sp.DtrEnable = $false
$sp.RtsEnable = $false

# The port can remain busy for a fraction of a second after esptool closes it.
$opened = $false
for ($attempt = 0; $attempt -lt 30; $attempt++) {
    try {
        $sp.Open()
        $opened = $true
        break
    } catch {
        Start-Sleep -Milliseconds 250
    }
}
if (-not $opened) {
    Write-Host "ERROR: cannot open ${portName} after retrying."
    Write-Host "Close any other serial monitor and run R1_MONITOR_COM9.bat again."
    exit 4
}

try {
    while ($true) {
        try {
            $s = $sp.ReadExisting()
            if ($s.Length -gt 0) {
                [Console]::Write($s)
            }
        } catch [System.TimeoutException] {
        }
        Start-Sleep -Milliseconds 20
    }
} finally {
    if ($sp.IsOpen) { $sp.Close() }
}
