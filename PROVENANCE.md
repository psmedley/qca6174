# Provenance

| Part | Source | Licence |
|---|---|---|
| `qc_hdr.asm`, `qc_end.asm`, `qc_ndisx.asm`, most of `qc_lib.asm`, `qc_os2dd.h`, `qc_ndis.h`; the structure of `qc_strat.c`, `qc_ndis.c`, `qc_nic.c` | OS2World [DRV-NET-RTLUSBWifi](https://github.com/OS2World/DRV-NET-RTLUSBWifi) (RTL88EU / RTL8821AU drivers, 0.1.4a4), written from the IBM PDD Reference, the NDIS 2.0.1 specification and the Wireless LAN Monitor / wpa_supplicant GenMAC sources | GPL-2.0-only |
| Register map, reset and boot sequences, copy-engine, BMI, HTC and WMI-TLV message formats, firmware/board container formats, numeric WMI-TLV tags/commands/events | Linux `drivers/net/wireless/ath/ath10k` (pci.c, ce.c, hw.c/.h, bmi.c/.h, htc.c/.h, core.c, swap.c, targaddrs.h, wmi.h, wmi-tlv.c/.h) | ISC |
| HTT messages, rx descriptor layout (computed by compiling the structs with gcc), tx descriptor, WMI vdev/peer/key/management commands and events, numeric ids (computed from the enums) | Linux `ath10k` htt.h, htt_tx.c, htt_rx.c, rx_desc.h, mac.c, wmi.h, wmi-tlv.c/.h | ISC |
| Auth/association frame contents, RSN element, join/rejoin behaviour towards the supplicant | OS2World RTL8821AU driver (`rtl_rx.c`, `rtl_nic.c`) | GPL-2.0-only |
| PCI configuration mechanism #1 | PCI Local Bus Specification | - |
| DevHelp numbers and register conventions | IBM PDD Reference via the claude-os2-toolkit `os2ref/kernel-services.md` and `rt_lib.asm` | - |

The WMI-TLV enum values were computed from the ath10k headers with a script (sequential enum evaluation),
not typed by hand.

The firmware files (`firmware-6.bin`, `board-2.bin`) are Qualcomm's and come from linux-firmware; they are
not part of this repository.

The combined work is distributed under GPL-2.0-only (`COPYING`); the ISC-licensed ath10k parts are
compatible with that.
