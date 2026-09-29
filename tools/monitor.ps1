param(
    [string]$Preferred = "COM5",
    [int]$Baud = 115200
)

$ErrorActionPreference = "Continue"

function Find-CardputerPort {
    try {
        $serial = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue
        if ($serial) {
            $p = $serial | Where-Object { $_.DeviceID -eq $Preferred } | Select-Object -First 1
            if ($p) { return $p.DeviceID }

            $p = $serial | Where-Object {
                $_.PNPDeviceID -match "VID_303A" -or
                $_.Name -match "Espressif|USB Serial|USB JTAG" -or
                $_.Description -match "Espressif|USB Serial|USB JTAG"
            } | Select-Object -First 1
            if ($p) { return $p.DeviceID }
        }
    } catch {}

    try {
        $pnp = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
            Where-Object {
                $_.PNPDeviceID -match "VID_303A" -and
                $_.Name -match "\(COM[0-9]+\)"
            } |
            Select-Object -First 1

        if ($pnp -and $pnp.Name -match "\((COM[0-9]+)\)") {
            return $Matches[1]
        }
    } catch {}

    try {
        $names = [System.IO.Ports.SerialPort]::GetPortNames()
        if ($names -contains $Preferred) { return $Preferred }
        if ($names -contains "COM5") { return "COM5" }
        if ($names.Count -eq 1) { return $names[0] }
    } catch {}

    return $null
}

Write-Host "============================================================"
Write-Host "CCCP R2F MONITOR - native USB auto reconnect"
Write-Host "Preferred: $Preferred @ $Baud"
Write-Host "Ctrl-C to exit"
Write-Host "============================================================"

while ($true) {
    $port = $null
    while (-not $port) {
        $port = Find-CardputerPort
        if (-not $port) {
            Start-Sleep -Milliseconds 400
        }
    }

    $sp = $null
    try {
        Start-Sleep -Milliseconds 600
        $sp = New-Object System.IO.Ports.SerialPort $port,$Baud,'None',8,'One'
        $sp.Handshake = 'None'
        $sp.DtrEnable = $false
        $sp.RtsEnable = $false
        $sp.ReadTimeout = 400
        $sp.Open()
        Write-Host "OPEN: $port @ $Baud"

        while ($true) {
            try {
                $line = $sp.ReadLine()
                Write-Host $line
            } catch [System.TimeoutException] {}
        }
    } catch {
        Write-Host "Serial reconnect: $($_.Exception.Message)"
        Start-Sleep -Milliseconds 500
    } finally {
        try {
            if ($sp -and $sp.IsOpen) { $sp.Close() }
        } catch {}
    }
}
