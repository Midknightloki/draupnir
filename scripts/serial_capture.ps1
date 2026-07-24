# Background-safe serial capture (arduino-cli monitor doesn't work for this - see docs/BLE_Profile_Fetch_Debugging.md)
# Usage: powershell -ExecutionPolicy Bypass -File scripts\serial_capture.ps1 -Port COM5 -DurationSec 120 -LogPath serial_capture.log
param(
    [Parameter(Mandatory=$true)][string]$Port,
    [int]$Baud = 115200,
    [int]$DurationSec = 120,
    [string]$LogPath = "serial_capture.log"
)

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
# RTS must stay false - toggling it pulses EN (reset) on this board's native USB-CDC.
# DTR must be true - the ESP32 Arduino core's USBCDC gates transmit on the "terminal
# open" line state; with DTR low, Serial.print() calls are silently dropped.
$sp.DtrEnable = $true
$sp.RtsEnable = $false
$sp.ReadTimeout = 200
$sp.Open()

"CAPTURE START $(Get-Date -Format o) port=$Port baud=$Baud" | Set-Content $LogPath

$deadline = (Get-Date).AddSeconds($DurationSec)
$buf = New-Object System.Text.StringBuilder
while ((Get-Date) -lt $deadline) {
    try {
        $chunk = $sp.ReadExisting()
        if ($chunk.Length -gt 0) {
            [void]$buf.Append($chunk)
            $text = $buf.ToString()
            $lastNewline = $text.LastIndexOf("`n")
            if ($lastNewline -ge 0) {
                $complete = $text.Substring(0, $lastNewline)
                foreach ($line in $complete -split "`n") {
                    $stamped = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss.fff"), $line.TrimEnd("`r")
                    Add-Content -Path $LogPath -Value $stamped
                }
                $buf = New-Object System.Text.StringBuilder
                [void]$buf.Append($text.Substring($lastNewline + 1))
            }
        } else {
            Start-Sleep -Milliseconds 100
        }
    } catch {
        Add-Content -Path $LogPath -Value "CAPTURE ERROR: $_"
        break
    }
}

"CAPTURE END $(Get-Date -Format o)" | Add-Content -Path $LogPath
$sp.Close()
