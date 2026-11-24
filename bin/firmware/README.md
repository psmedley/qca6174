# Firmware files

The driver needs two files from linux-firmware (`ath10k/<chip>/<hw>/`) copied to `\DEV\WRND32$` at every boot
(see `install/STARTUP.CMD.example`):

| Folder | Card | linux-firmware source |
|---|---|---|
| `QCA6174/` | QCA6174 (`168C:003E`) | `ath10k/QCA6174/hw3.0/firmware-6.bin`, `board-2.bin` |
| `QCA9377/` | QCA9377 (`168C:0042`), not added yet | `ath10k/QCA9377/hw1.0/firmware-6.bin`, `board-2.bin` |

These are Qualcomm Atheros binaries redistributed under the linux-firmware licence (`WHENCE` / `LICENCE.atheros_firmware`
in the linux-firmware tree); check that licence before redistributing them further. Do not mix the two sets.
