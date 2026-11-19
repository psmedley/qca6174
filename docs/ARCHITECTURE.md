# Architecture - QCA6174 for OS/2

## 1. Big picture

```
 Wireless LAN Monitor / wpa_supplicant              TCP/IP (NDIS 2.0.1 protocol)
        | DosDevIOCtl cat 99h (GenMAC OIDs)                 | TransmitChain / ReceiveChain
        v                                                    v
   qc_nic.c  (OID layer)                             qc_ndis.c (MAC tables, indications)
        \                                                   /
         +------------ ath_sta.c (scan table; phase 2: join, keys, data) ----+
                               |
                ath_wmi.c (WMI-TLV)       [phase 2: ath_htt.c (HTT data)]
                               |
                ath_htc.c (HTC endpoints, credits, service loop)
                               |
                ath_ce.c (copy engines, diagnostic window)   ath_boot.c (reset, BMI, firmware download)
                               |
                qc_pci.c (PCI config, BAR0 MMIO, IRQ, context hook)   qc_mem.c (DMA memory, firmware blobs)
                               |
                         QCA6174 (PCIe)
```

One 16-bit PDD, character device `WRND32$`, the same container and segment layout as the RTL88EU driver
(NE "dll", IOPL code segment, header first, resident-end markers last).

## 2. Source map

| File | Role | Origin |
|---|---|---|
| `qc_hdr.asm`, `qc_end.asm` | device header, segment order, strategy entry; resident end | RTL88EU `rt_hdr.asm`, `rt_end.asm` |
| `qc_lib.asm` | DevHelp wrappers, SMP locks, context-hook entry; **new:** 32-bit MMIO/port access, SetIRQ/EOI, the interrupt entry | RTL88EU `rt_lib.asm` + new |
| `qc_ndisx.asm`, `qc_ndis.c/.h` | NDIS 2.0.1 MAC | RTL88EU `rt_ndisx.asm`, `rt_ndis.c/.h` |
| `qc_strat.c` | strategy routine, INIT, clock | RTL88EU `rt_strat.c` |
| `qc_nic.c` | GenMAC OID device | RTL88EU `rtl_nic.c` |
| `qc_pci.c` | PCI configuration (mechanism #1), BAR0 windows, interrupt handler, context hook | new |
| `qc_mem.c` | GDT selectors, DMA chunks, scratch mappings, firmware blobs | new |
| `qc_fw.c` | `firmware-N.bin` / `board-2.bin` containers | ath10k `core.c` |
| `ath_ce.c` | copy engines, diagnostic window | ath10k `ce.c`, `pci.c` |
| `ath_boot.c` | wake, cold/warm reset, `init_config`, BMI, board data, OTP, code swap, firmware download, the bring-up sequence | ath10k `pci.c`, `bmi.c`, `core.c`, `swap.c` |
| `ath_htc.c` | HTC and the service loop | ath10k `htc.c`, `pci.c` |
| `ath_wmi.c` | WMI-TLV: init, ready, regdomain, channel list, vdev, scan | ath10k `wmi-tlv.c`, `wmi.c`, `mac.c` |
| `ath_sta.c` | scan table; phase-2 stubs | new |
| `qc_dbg.c` | COM1 trace, firmware upload, status text, debug commands | new |

## 3. Memory

* **BAR0** (2 MB of registers) is mapped by 32 GDT selectors of 64 KB (PhysToGDTSelector). Every register
  access is one 32-bit bus cycle (`qc_rd32`/`qc_wr32`): 16-bit C would split it into two 16-bit cycles,
  which the chip does not accept. The device is kept awake for good (`PCIE_SOC_WAKE`), as ath10k does
  with `pci_ps` off, so there is no per-access wake/sleep.
* **DMA memory**: 12 chunks of 64 KB from AllocPhys at INIT, each with a permanent GDT selector; rings and
  buffers are carved out of them. Host ring sizes are smaller than Linux's (see the table in `ath_ce.c`).
* **Firmware host memory** that the firmware asks for in the service-ready event (`mem_reqs`) and the
  code-swap segment are separate AllocPhys blocks; the driver never touches them after zeroing.
* **Firmware files** are written to the device at run time and kept as lists of 64 KB physical blocks
  (`BLOB`); they need not be contiguous because only the driver reads them (through a scratch selector).
* DGROUP is about 17 KB.

## 4. Execution contexts

* **Strategy routine (task time)**: the GenMAC IOCtl path holds `nic_lock` (ProcBlock-based, as in the RTL
  driver). Chip bring-up, WMI commands and scans run here and may block (ProcBlock with a timeout).
* **Interrupt** (`qc_irq_entry` -> `qc_irq_c`): checks `PCIE_INTR_CAUSE`, masks and clears the legacy
  interrupt, issues the EOI, arms the context hook. Shared IRQ: returns "not mine" when the cause is clear.
* **Context hook** (task time, must not block): `ath_service()` (all copy engines), re-enables the
  interrupt, delivers queued receive frames to NDIS.
* `ath_service()` is guarded by an owner-tagged lock; a caller that finds it busy leaves a note and the
  owner runs once more. Copy-engine sends are serialised by `nic_lock` (phase 2 will need a send lock for
  NDIS TransmitChain, which may arrive at interrupt time).
* Without a working interrupt the driver still runs: every GenMAC request and every wait polls the
  copy engines.

## 5. Boot sequence (`ath_up`, `g_hw_state` = step number in the status text)

1 files present, 2 parse firmware, 3 PCI enable (memory, bus master, ASPM off), 4 map BAR0, 5 copy-engine
memory, 6-7 BMI buffers, 8 force wake, 9 cold reset + warm reset, 10 chip id (rev 8/9/10 = hw3.0/3.1/3.2),
11 `init_config` (CE configuration and service map into the target over the diagnostic CE), 12 BMI target
info, 13 board data lookup (OTP board id, else PCI subsystem ids), 14 target options, 15 board data,
16 OTP, 17 code swap, 18 firmware (BMI LZ stream to 0x1234), 19 UART off, 20 BMI done, then interrupts and
receive buffers, 21 HTC READY, 22 HTT service, 23 WMI service, 24 HTC setup complete, 25 WMI: service ready,
host memory, INIT, READY (MAC address), regdomain, channel list, station vdev. 100 = ready.

ath10k on hw3.2 also programs the PLL and downloads the firmware through the diagnostic copy engine; both are
speed-ups with a BMI fallback in Linux, so this driver uses plain BMI.

## 6. GenMAC interface

As in the RTL driver (see its ARCHITECTURE.md, section 6): IOCtl category 99h, functions 40h/41h, parameter
packet of ULONGs (OID, length, NDIS status, bytes, NDIS status again); private OIDs `FFFFFF00` (lanN),
`01` (wireless), `02` (link), `08` (NIF name), `09` (GenMAC version >= 2.00), `0B`..`0E` (EAPOL).

## 7. Open questions - to check against the DDK / on hardware

These are the places where the code rests on an assumption that could not be sourced while it was written.
Each is marked in the code.

1. **Interrupt handler contract** (`qc_lib.asm`, `qc_irq_entry`): entered by a far call, returns with RETF,
   carry clear = handled (EOI issued), carry set = not mine. Check against the PDD reference ("SetIRQ",
   "Interrupt Handler"). Also whether SetIRQ accepts IRQ numbers above 15 under the ACPI PSD (PCIe cards
   often get 16-23); if not, the driver falls back to polling (`[no irq: polled]` on COM1).
2. **PhysToGDTSelector** at INIT time and with length 0FFFFh for MMIO (BAR0) - the code retries the mapping at
   bring-up if INIT refuses. Also: is the mapping uncached? The BIOS MTRRs normally make the PCI hole UC.
3. **AllocPhys at task time** (firmware blobs during `copy /b`, firmware host memory, code swap).
4. **PCI configuration space** through ports CF8h/CFCh instead of OEMHLP$ (category 80h, function 0Bh): the
   OEMHLP$ packet layout was not available (EDM2 was unreachable). On SMP another CPU could use CF8h at the
   same moment; the accesses happen only at INIT and bring-up.
5. **Regulatory domain**: the EEPROM regdomain is passed to the firmware as is, with CTL_FCC (10h) for both
   bands. ath10k maps the EEPROM value through the ath `regd.c` tables to a regpair and its CTLs; that table
   is not ported yet. Scan works either way; transmit power limits may be off.
6. **Board name**: `bus=pci,bmi-chip-id=N,bmi-board-id=M` when the OTP gives a board id, else
   `bus=pci,vendor=168c,device=003e,subsystem-vendor=xxxx,subsystem-device=yyyy`. ath10k also tries a
   `variant=` from SMBIOS; not done here. If no entry matches, the status text shows the name that was
   looked for.

## Phase 2

The join and data path, in the order ath10k does it for a station (mac.c, wmi-tlv.c, htt_tx.c, htt_rx.c):

1. **HTT setup**: version request, RX ring configuration (`HTT_H2T_MSG_TYPE_RX_RING_CFG`: a ring of 1920-byte
   receive buffers in DMA memory and an "alloc index" word), refill. Set WMI `rx_decap_mode` to native WiFi
   (as now) or try Ethernet decap to avoid 802.11 <-> 802.3 conversion in the driver.
2. **Join**: `WMI_VDEV_START_REQUEST` on the AP's channel, `WMI_PEER_CREATE` for the BSSID, authentication and
   association frames (management TX over WMI or HTT, per the firmware's services), then `WMI_PEER_ASSOC`
   (rates, HT/VHT capabilities) and `WMI_VDEV_UP` (AID, BSSID).
3. **Keys**: `WMI_VDEV_INSTALL_KEY` (pairwise and group CCMP) from `OID_802_11_ADD_KEY`, then the peer
   `AUTHORIZE` parameter. EAPOL frames go to the supplicant through the private OIDs `0C`/`0D`, and its
   replies are sent with `0E` (as in the RTL driver).
4. **Data**: HTT TX descriptors + fragment lists on CE4 (with a larger host ring), TX completions; HTT RX
   indications on CE1 -> 802.3 frames -> `ndis_rx_put()`.
5. A send lock for copy-engine source rings, so that NDIS TransmitChain can run beside the request path.
