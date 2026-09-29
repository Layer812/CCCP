param(
    [string]$Preferred = "COM5"
)

$ErrorActionPreference = "SilentlyContinue"

function Get-CardputerPort {
    # 1) Normal Win32_SerialPort path.
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

    # 2) Native USB Serial/JTAG sometimes appears more reliably as a PnP entity.
    $pnp = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object {
            $_.PNPDeviceID -match "VID_303A" -and
            $_.Name -match "\(COM[0-9]+\)"
        } |
        Select-Object -First 1

    if ($pnp -and $pnp.Name -match "\((COM[0-9]+)\)") {
        return $Matches[1]
    }

    # 3) Last-resort enumerated COM ports.
    $names = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($names -contains $Preferred) { return $Preferred }
    if ($names -contains "COM5") { return "COM5" }
    if ($names.Count -eq 1) { return $names[0] }

    return $null
}

$port = Get-CardputerPort
if ($port) {
    Write-Output $port
    exit 0
}

exit 2
