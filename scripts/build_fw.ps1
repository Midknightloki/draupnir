# Compile the M5_M6_config firmware and log all output to build_fw.log
# Usage: powershell -ExecutionPolicy Bypass -File scripts\build_fw.ps1
$repo = "F:\Projects\Draupnir\draupnir"
$log = Join-Path $repo "build_fw.log"
"BUILD START $(Get-Date -Format o)" | Set-Content $log
& "$repo\arduino-cli.exe" compile `
  --fqbn "m5stack:esp32:m5stack_dial:USBMode=default,CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=default_8MB" `
  "$repo\firmware\M5_M6_config" *>&1 | Add-Content $log
"EXIT:$LASTEXITCODE" | Add-Content $log
