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

## 8. Phase 2: join, keys and data

ath10k leaves the 802.11 state machine to mac80211. This driver has no mac80211, so `ath_sta.c` does what
mac80211 does for a station, in the order ath10k's `mac.c` drives the firmware:

1. **vdev start** on the AP's channel (`WMI_VDEV_START_REQUEST`, legacy 20 MHz mode), wait for the response event.
2. **peer create** for the BSSID (the HTT PEER_MAP event confirms it; a missing event is logged and ignored).
3. **authentication** (open system) and **association request** built by the driver and sent as management frames
   with `WMI_MGMT_TX`; the answers arrive as `WMI_MGMT_RX` events (`sta_mgmt_rx`). Three attempts each.
   The request carries a fixed RSN element (WPA2-PSK / CCMP) for a secure network.
4. **peer assoc** (`WMI_PEER_ASSOC`: AID, capabilities, the AP's legacy rates; no HT/VHT) and **vdev up**.
5. **keys**: `OID_802_11_ADD_KEY` becomes `WMI_VDEV_INSTALL_KEY` (CCMP; pairwise on the BSSID, group by index);
   after the pairwise key the peer is authorized (`WMI_PEER_AUTHORIZE`). On an open network the peer is authorized
   at once.
6. **leave**: deauthentication frame, vdev down, peer delete, vdev stop.

The join runs inside the GenMAC request (`OID_802_11_SSID`); the supplicant's re-join trick of the RTL driver
(leave, then join from a request 1.5 s later) is kept because the supplicant only notices an association when the
BSSID it polls changes. A deauthentication, a disassociation or a firmware kick-out sets `g_link_lost`; the next
request cleans up (`sta_poll`).

### Data path (`ath_htt.c`)

* **Setup**: HTT version request/confirmation, RX ring configuration, aggregation limits. Requires the firmware
  service `RX_FULL_REORDER` (QCA6174 firmware has it); the legacy RX_IND path is not implemented and the bring-up
  stops with a message if the service is missing.
* **Receive**: a ring of 128 slots with 62 buffers of 2 KB (two 64 KB DMA chunks). The firmware reports filled
  buffers, in order, in `RX_IN_ORD_PADDR_IND`. Each buffer = 300-byte rx descriptor (layout computed from the
  ath10k structs with gcc: attention at 4, msdu_start at 24, rx_hdr_status at 236, payload at 300) + the MSDU as an
  802.11 "native wifi" frame. `rx_msdu` checks the attention flags, strips the 802.11 header (24/26/30/32 bytes,
  rounded up to 4 unless the firmware feature `NO_NWIFI_DECAP_4ADDR_PADDING` is set) and the LLC/SNAP header, and
  hands an Ethernet frame to `sta_rx_eth`. EAPOL (0x888E) goes to a four-frame queue read by the supplicant
  (`OID 0xFFFFFF0C/0D`), everything else to NDIS (`ndis_rx_put`, delivered from the context hook).
* **Transmit**: `htt_tx_eth` builds the 802.11 data frame (To-DS, LLC/SNAP; Protected bit once the pairwise key
  is in) in one of 24 slots and queues two chained copy-engine entries on CE4. EAPOL frames go out unprotected
  with NO_ENCRYPT. Completions (`TX_COMPL_IND`) free the slot; a slot whose completion never comes is reclaimed
  after 3 s.

## 9. Open questions - to check on hardware (phase 2)

These are the places where the code follows ath10k from reading it, with nothing to test it against yet.

1. **HTT RX ring start index.** `fw_idx_init_val` is set to the alloc index after the first refill, as ath10k
   does (the doc comment in ath10k's `htt.h` describes the alloc index as the position the host has filled up to).
   If the firmware never reports a buffer, look at `RXIND=` in the status text and at this first.
2. **RX descriptor layout.** The first WPA2 attempt associated (AID assigned) but every received frame was dropped:
   the decap format was read from byte +5 of `rx_msdu_start`, but `struct rx_msdu_start_common` is
   `{ info0; flow_id_crc; info1 }`, so it is at +8 (bits 9:8). Fixed; `RXWHY=` in the status text counts the reasons a
   frame can be dropped (0 unknown buffer, 1 MSDU not done, 2 error flags, 3 decap format, 4 not data, 5 too short,
   6 too long, 7 not connected / offloaded) and `LAST` shows the last decap, frame-control byte and attention word.
   The remaining offsets are for `htt_rx_desc_v1`; the offsets the driver sends in the ring
   configuration and the ones it reads must agree with the firmware (`RXDROP=` counts frames rejected for a
   missing MSDU_DONE flag, errors, or an unknown buffer address).
3. **Management frames.** ath10k sends them over WMI only if the firmware has the feature "has-wmi-mgmt-tx" (bit 2)
   or the service MGMT_TX_WMI (76); otherwise through the HTT data path with packet type MGMT (HTT >= 3). The
   driver makes the same choice (`MGMT=wmi|htt` in the status text). The first hardware run reported
   `MGMTWMI=0`, so which path is used depends on the feature bits (`FEAT=` shows them). Either path is untested;
   if every join ends with `JFAIL=2` (no authentication reply), this is the first suspect.
4. **vdev start details.** Beacon interval and DTIM are taken from the beacon; the channel is sent as a legacy
   20 MHz channel. ath10k also sets a number of vdev and peer parameters (RTS threshold, power save, kick-out
   timers) that are not set here; the firmware defaults are assumed.
5. **The hardware RX filter workaround** ath10k does at start-up for this chip (create and delete a dummy vdev so
   that `MAC_PCU_ADDR1_MASK` is set) is not done. Symptom if it matters: the card ACKs frames for other stations.
6. **Throughput.** Legacy rates only (no HT/VHT, no aggregation requests from our side), 20 MHz. The AP's
   aggregation towards us is handled by the firmware's reorder.
7. **No WPA3, WPA1, TKIP, WEP, 802.11w, PMKID caching.** WPA2-PSK/CCMP and open networks only, like the RTL
   drivers.
 for `htt_rx_desc_v1`; the offsets the driver sends in the ring
   configuration and the ones it reads must agree with the firmware (`RXDROP=` counts frames rejected for a
   missing MSDU_DONE flag, errors, or an unknown buffer address).
3. **Management frames.** ath10k sends them over WMI only if the firmware has the feature "has-wmi-mgmt-tx" (bit 2)
   or the service MGMT_TX_WMI (76); otherwise through the HTT data path with packet type MGMT (HTT >= 3). The
   driver makes the same choice (`MGMT=wmi|htt` in the status text). The first hardware run reported
   `MGMTWMI=0`, so which path is used depends on the feature bits (`FEAT=` shows them). Either path is untested;
   if every join ends with `JFAIL=2` (no authentication reply), this is the first suspect.
4. **vdev start details.** Beacon interval and DTIM are taken from the beacon; the channel is sent as a legacy
   20 MHz channel. ath10k also sets a number of vdev and peer parameters (RTS threshold, power save, kick-out
   timers) that are not set here; the firmware defaults are assumed.
5. **The hardware RX filter workaround** ath10k does at start-up for this chip (create and delete a dummy vdev so
   that `MAC_PCU_ADDR1_MASK` is set) is not done. Symptom if it matters: the card ACKs frames for other stations.
6. **Throughput.** Legacy rates only (no HT/VHT, no aggregation requests from our side), 20 MHz. The AP's
   aggregation towards us is handled by the firmware's reorder.
7. **No WPA3, WPA1, TKIP, WEP, 802.11w, PMKID caching.** WPA2-PSK/CCMP and open networks only, like the RTL
   drivers.
