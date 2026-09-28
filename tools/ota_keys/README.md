# OTA signing keys

Firmware images are signed with ECDSA P-256. The device holds the public key; only someone with
the private key can push firmware over BLE.

## Generating a key

    openssl ecparam -name prime256v1 -genkey -noout -out tools/ota_keys/ota_private.pem
    python tools/sign_firmware.py --emit-pubkey

The first command writes a private key that is gitignored. The second regenerates
`firmware/Waveshare_LVGL_Test/ota_pubkey.h`, which IS committed.

## Where the private key lives

**Not in this repo.** It is gitignored, and the repo is public. Before the first public release it
needs a real home — a password manager, a hardware token, or an offline backup — and at least one
copy that does not live on the build machine.

**Losing it ends OTA for every shipped unit.** They would keep working, and would still be
flashable over USB, but no signed update could ever be produced for them again. Rotating to a new
key requires a firmware update signed with the OLD key, so the window to recover from a loss
closes the moment the key is gone.

## Using it

`sign_firmware.py` reads the path from `DRAUPNIR_OTA_KEY`. It has no default and does not search
— signing with whatever key happened to be lying around is exactly the failure this avoids.
