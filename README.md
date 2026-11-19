# QCA6174 for OS/2 and ArcaOS

A native driver for the **Qualcomm Atheros QCA6174 802.11ac PCIe WiFi adapter** (`168C:003E`, the chip in
the Killer 1525/1535, Dell DW1820 and many laptop M.2 cards) for OS/2 and ArcaOS. It has the same shape as
the OS2World [RTL88EU / RTL8821AU drivers](https://github.com/OS2World/DRV-NET-RTLUSBWifi), whose OS/2
plumbing it reuses:

* a **GenMAC-compatible device** (`\DEV\WRND32$`, DosDevIOCtl category 99h OID GET/SET), so the ArcaOS
  **Wireless LAN Monitor** (XWLAN) and `wpa_supplicant` can drive it, and
* an **NDIS 2.0.1 MAC** (`QCA6174_NIF`), so the OS/2 TCP/IP stack binds to it as `lanN`.

The chip side is a port of the Linux **ath10k** driver (PCI bus, copy engines, BMI, HTC, WMI-TLV). It is a
16-bit physical device driver built with Open Watcom 2.0 and NASM; no IBM DDK is needed.

> **Status: phase 1, untested on hardware.** The code builds (cross-built on Linux) but has not yet been
> run on a machine with a QCA6174. Read [docs/TESTING.md](docs/TESTING.md) before trying it, and expect to
> need a COM1 cable or a VM serial port.

## What phase 1 is meant to do

| Step | Code | Status |
|---|---|---|
| Find the card on the PCI bus, map BAR0, DMA memory | `qc_pci.c`, `qc_mem.c` | written |
| Take `firmware-6.bin` / `board-2.bin` written to `\DEV\WRND32$` | `qc_dbg.c`, `qc_fw.c` | written |
| Cold + warm reset, copy-engine rings, target configuration | `ath_boot.c`, `ath_ce.c` | written |
| Board data (by OTP board id or PCI subsystem id), OTP, firmware download over BMI | `ath_boot.c` | written |
| HTC: wait for READY, connect HTT and WMI services | `ath_htc.c` | written |
| WMI-TLV: service ready, host memory for the firmware, INIT, READY (MAC address), regdomain, channel list, station vdev | `ath_wmi.c` | written |
| Scan (2.4 GHz and 5 GHz), BSS list to the Wireless LAN Monitor | `ath_wmi.c`, `ath_sta.c`, `qc_nic.c` | written |
| Join, WPA2 keys, EAPOL, data (HTT TX/RX) | - | **phase 2**, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#phase-2) |

In phase 1 a join request (`OID_802_11_SSID`) fails with `NDIS_STATUS_FAILURE` and transmitted frames are
dropped and counted, so the Wireless LAN Monitor shows networks but cannot connect.

## Install (for testing)

1. Copy `bin\QCA6174.OS2` and `install\QCA6174.NIF` to `C:\IBMCOM\MACS`.
2. Get `firmware-6.bin` and `board-2.bin` from linux-firmware, directory `ath10k/QCA6174/hw3.0/` (hw3.0 and
   hw3.2 chips use the same files), and copy them to `C:\IBMCOM\MACS\QCA6174\`.
3. `CONFIG.SYS`: `DEVICE=C:\IBMCOM\MACS\QCA6174.OS2 /V` (only one `WRND32$` driver can be loaded: remove
   GenMAC or an RTL driver line).
4. `PROTOCOL.INI`: bind `QCA6174_NIF` as in `install\PROTOCOL.INI.example`.
5. `STARTUP.CMD`: the two `copy /b` lines from `install\STARTUP.CMD.example`.
6. Reboot. `copy \DEV\WRND32$ con` prints the driver state; `echo up > \DEV\WRND32$` starts the adapter and
   `echo scan > \DEV\WRND32$` scans, with progress on COM1.

## Building

Linux (cross): put Open Watcom 2.0 `binl64` and NASM on the `PATH`, set `WATCOM`, then `make -C src`.
OS/2: `src\build.cmd`. Flags and constraints are those of the RTL driver (`-2 -ms -zu -zl -zp1`, no 32-bit
multiply/divide/variable shift, DGROUP below 64 KB). See [BUILD.md](BUILD.md).

## Licence

GPL-2.0-only (`COPYING`). The OS/2 plumbing derives from the OS2World RTL88EU driver (GPL-2.0-only); the chip
code is ported from Linux ath10k (ISC). See [PROVENANCE.md](PROVENANCE.md).
