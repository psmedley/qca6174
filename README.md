# QCA6174 / QCA9377 for OS/2 and ArcaOS

A native driver for Qualcomm Atheros **ath10k** PCIe 802.11ac WiFi adapters on OS/2 and ArcaOS:

| Card | PCI ID | Status |
|---|---|---|
| QCA6174 (hw3.x; Killer 1525/1535, Dell DW1820 and many laptop M.2 cards) | `168C:003E` | working |
| QCA9377 (hw1.0 / 1.1, 1x1) | `168C:0042` | working (one report, see below) |

It has the same shape as the OS2World [RTL88EU / RTL8821AU drivers](https://github.com/OS2World/DRV-NET-RTLUSBWifi),
whose OS/2 plumbing it reuses:

* a **GenMAC-compatible device** (`\DEV\WRND32$`, DosDevIOCtl category 99h OID GET/SET), so the ArcaOS
  **Wireless LAN Monitor** (XWLAN) and `wpa_supplicant` can drive it, and
* an **NDIS 2.0.1 MAC** (`QCA6174_NIF`), so the OS/2 TCP/IP stack binds to it as `lanN`.

The chip side is a port of the Linux **ath10k** driver (PCI bus, copy engines, BMI, HTC, WMI-TLV, HTT). It is a
16-bit physical device driver built with Open Watcom 2.0 and NASM; no IBM DDK is needed.

## What works

Tested on ArcaOS (SMP kernel):

* QCA6174 in a Dell Latitude 7390 2-in-1: scan (2.4 and 5 GHz), WPA2-PSK join with `wpa_supplicant`, DHCP and static IP,
  HT20/HT40/VHT80 on 5 GHz, up to about 8.5 MB/s from a wired server.
* QCA9377 in a Lenovo Miix 510-12ISK (issue #1, thanks to the reporter): works with no source change; scan on both bands,
  WPA2-PSK, DHCP, ping, ssh; 1 spatial stream, about 2.5-3 MB/s on a strong link. See `docs/TESTING.md` section 19.

Not done: WPA3 (SAE), WPA1/TKIP/WEP, 160 MHz, power save (turned off), roaming, AP mode. Other ath10k chips (QCA988x, QCA9984,
QCA99x0, ...) use other firmware protocols and need more work. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
[docs/NEXT.md](docs/NEXT.md).

## Install

1. Copy `bin\QCA6174.OS2` and `install\QCA6174.NIF` to `C:\IBMCOM\MACS`.
2. Firmware: copy the two files for your card from `bin\firmware\` (or linux-firmware, `ath10k/QCA6174/hw3.0/` or
   `ath10k/QCA9377/hw1.0/`) to `C:\IBMCOM\MACS\QCA6174\`. Do not mix the sets (see `bin\firmware\README.md`).
3. `CONFIG.SYS`: `DEVICE=C:\IBMCOM\MACS\QCA6174.OS2 /V` (only one `WRND32$` driver can be loaded: remove
   GenMAC or an RTL driver line).
4. `PROTOCOL.INI`: bind `QCA6174_NIF` as in `install\PROTOCOL.INI.example` (the NIF name is the same for both cards).
5. `STARTUP.CMD`: the two `copy /b` lines from `install\STARTUP.CMD.example`. They must run **before** the supplicant or the
   Wireless LAN Monitor starts; otherwise the adapter has MAC `00:00:00:00:00:00` and the WPA2 handshake fails.
6. The MAC address is only known once the firmware runs, which is after the protocol stack has bound. Reboot once, run
   `copy \DEV\WRND32$ con` and look for `FWMAC=`, then either run `install\QCMAC.CMD` (it writes `NETADDRESS` into
   `PROTOCOL.INI`, keeping a `.bak`) or add `NETADDRESS = "xxxxxxxxxxxx"` under `[QCA6174_NIF]` by hand, and reboot again.
   A different card needs this step again.

`copy \DEV\WRND32$ con` prints the driver state at any time; `echo up > \DEV\WRND32$` starts the adapter and
`echo scan > \DEV\WRND32$` scans (progress on COM1).

## PROTOCOL.INI settings (`[QCA6174_NIF]`)

| Setting | Meaning |
|---|---|
| `NETADDRESS = "xxxxxxxxxxxx"` | The adapter's MAC address (12 hex digits). Needed, see Install step 6. |
| `CHANNEL = n` | Join only an access point whose primary channel is `n` (for example to prefer 5 GHz). |
| `MAXWIDTH = 20 \| 40 \| 80` | Widest channel to use (default 80). |
| `DEDUP = ON` | Drop IPv4 frames identical to one of the last 64 delivered (works around networks that deliver every packet twice). Off by default. |

## Notes

* When benchmarking, download from the server's LAN address, not its public name: a router that loops traffic back
  (NAT loopback) can deliver every packet twice and look like a slow driver (`docs/TESTING.md` section 15).
* WPA3-only networks do not work (no SAE); a WPA3 transition-mode SSID joins as WPA2-PSK.
* The driver picks the strongest access point with the matching SSID; use `CHANNEL` to steer it.
* Status values, beeps (`/B`) and test steps: [docs/TESTING.md](docs/TESTING.md).

## Building

Linux (cross): put Open Watcom 2.0 `binl64` and NASM on the `PATH`, set `WATCOM`, then `make -C src`.
OS/2: `src\build.cmd`. Flags and constraints are those of the RTL driver (`-2 -ms -zu -zl -zp1`, no 32-bit
multiply/divide/variable shift, DGROUP below 64 KB). See [BUILD.md](BUILD.md).

## Licence

GPL-2.0-only (`COPYING`). The OS/2 plumbing derives from the OS2World RTL88EU driver (GPL-2.0-only); the chip
code is ported from Linux ath10k (ISC). The firmware files are Qualcomm Atheros binaries under the linux-firmware licence.
See [PROVENANCE.md](PROVENANCE.md).
