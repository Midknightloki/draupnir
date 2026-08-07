# Background-safe serial capture (arduino-cli monitor doesn't work for this - see docs/BLE_Profile_Fetch_Debugging.md)
# Usage: powershell -ExecutionPolicy Bypass -File scripts\serial_capture.ps1 -Port COM5 -DurationSec 120 -LogPath serial_capture.log
param(
    [Parameter(Mandatory=$true)][string]$Port,
    [int]$Baud = 115200,
    [int]$DurationSec = 120,
    [string]$LogPath = "serial_capture.log"
)

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
# DTR must be true - USBCDC::write() drops output unless tud_cdc_n_connected() is true, which
# tracks DTR. With DTR low, Serial.print() is silently discarded. RTS does not affect output.
#
# THESE SETTINGS DO NOT, BY THEMSELVES, PREVENT A RESET. An earlier comment here claimed RTS
# "pulses EN"; that is the wrong mechanism and it made this script look safe when it was not.
# The real one is a 4-state DTR/RTS machine in the core's USBCDC::_onLineState():
#     !dtr&&rts  ->  dtr&&rts  ->  dtr&&!rts  ->  !dtr&&!rts  ->  RESTART_BOOTLOADER
# Opening a .NET SerialPort walks the first three regardless of what is requested here (RTS
# asserts transiently on open, DTR then applies, RTS then settles), and Close() supplies the
# fourth. So the board reset into download mode on CLOSE -- meaning the guilty capture always
# looked fine and the NEXT run failed with "The port is closed".
#
# The fix is firmware-side: Serial.enableReboot(false) in Waveshare_LVGL_Test.ino disarms that
# machine entirely. This script is only safe against firmware carrying that call. If you capture
# from a board WITHOUT it (e.g. firmware/M5_M6_config), expect the reset and budget a replug.
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
