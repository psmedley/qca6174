# Testing phase 1

The driver has not run on hardware yet. This is the plan for the first runs; it relies on the COM1 trace
(115200/9600 baud is whatever the kernel debugger or the BIOS left programmed - the driver only writes the
transmit register) and on the status text that a read of `\DEV\WRND32$` returns.

Use a machine where the WiFi card is not needed to boot, keep a working CONFIG.SYS to fall back to, and
remove GenMAC / other `WRND32$` drivers first.

## 1. Load

`DEVICE=C:\IBMCOM\MACS\QCA6174.OS2 /V`. Expected on screen and COM1:

```
I0 I1 I2 I3
QCA6174: Qualcomm Atheros QCA6174 802.11ac PCIe WLAN driver for OS/2.
QCA6174: NDIS MAC registered with the Protocol Manager.
```

`copy \DEV\WRND32$ con` then shows `PCI=`, the vendor/device/subsystem ids, `REV=32` for the card in
question (hw3.2), the IRQ and BAR0. If the driver refuses to load, the message says why (no card, BAR above
4 GB, no memory).

## 2. Firmware upload

```
copy /b C:\IBMCOM\MACS\QCA6174\firmware-6.bin \DEV\WRND32$
copy /b C:\IBMCOM\MACS\QCA6174\board-2.bin \DEV\WRND32$
copy \DEV\WRND32$ con
```

COM1: `[fw 000xxxxx loaded]`, `[board 000xxxxx loaded]`. The status shows both lengths as `loaded`.

## 3. Bring-up

`echo up > \DEV\WRND32$`. COM1 shows `[up]`, then `[ready]`, or `[QCA6174: <what failed> <value>]`. The
status text gives `STATE=` (the step reached, see ARCHITECTURE.md section 5), `CHIP=`, `TARGET=`
(`05030000` expected for hw3.2), `FWVER=`, `BOARDNAME=`, `MAC=`.

What to look at when it stops:

| STATE | Meaning | Look at |
|---|---|---|
| 8 | the chip does not wake | BAR0 mapping (`echo r 80000 > \DEV\WRND32$` reads RTC_STATE) |
| 9 | reset / target init | `LASTERR` (FW_INDICATOR value) |
| 11 | diagnostic copy engine | ring programming, DMA addresses |
| 12-20 | BMI | CE0/CE1, the BMI timeout |
| 13 | board data | `BOARDNAME`: is that name in board-2.bin? |
| 21 | no HTC READY | interrupts/polling, CE1 receive buffers |
| 25 | WMI | `WMIEV` counter, `LASTERR` |

## 4. Scan

`echo scan > \DEV\WRND32$`, then `copy \DEV\WRND32$ con`: `NETS=` and one `NET=channel rssi ssid` line per
network. Then start the Wireless LAN Monitor: it should list the same networks.

## 5. Interrupts

`IRQS=` should grow while the adapter is up; `SPUR=` counts interrupts on a shared line that were not ours.
`IRQS=0` with networks found means the driver is running polled (see ARCHITECTURE.md, open question 1).

## 6. Join an open network (phase 2, no supplicant)

On a test access point with encryption off:

```
echo scan > \DEV\WRND32$
echo join MyOpenNetwork > \DEV\WRND32$
copy \DEV\WRND32$ con
```

COM1 shows `[join rc 0000 fail 0000]` and `[joined aid nnnn]`. The status text has `LINK=JOINED ... OPEN
AUTHORIZED`. `JFAIL`: 1 SSID not in the scan table, 2 no authentication reply, 3 authentication refused,
4 no association reply, 5 association refused (`ASSOC=` is the status code), 6 wrong security mode,
9 vdev start failed, 10 a command could not be sent. `echo leave > \DEV\WRND32$` disconnects.

With the NDIS interface up (`lanN` configured as for the RTL driver, a static address is simplest), `ping` the
access point. `HTT=ready`, `RXIND` (receive indications), `RXFRM`, `TXOK` and `TXCOMPL` should all grow; `RXDROP`
and `TXFULL` should stay small.

## 7. WPA2-PSK through the Wireless LAN Monitor

Create a profile as for the RTL driver and connect. `EAPOL=` shows queued handshake frames (normally 0 again once
the supplicant has read them); after the handshake the status shows `PTK AUTHORIZED`. Failures before that point
are visible as `JFAIL`; a handshake that never completes with `RXFRM` stuck at 0 points at the receive ring
(ARCHITECTURE.md section 9, question 1).

## 8. The boot hangs and there is no serial port

The driver can report how far INIT got by beeping, and can stop INIT early so the boot gets past the hang.

**Beeps.** Add `/B` to the `DEVICE=` line: `DEVICE=C:\IBMCOM\MACS\QCA6174.OS2 /B`. At each step the driver sounds
that many short beeps (1 kHz) and then pauses; count the LAST group before the machine stops:

| beeps | step reached |
|---|---|
| 1 | INIT entered, arguments read |
| 2 | DMA memory and GDT selectors allocated |
| 3 | PCI scan done (the card was found) |
| 4 | PCI scan result checked (nothing is mapped at INIT any more) |
| long + 1 | inside step 5: (the NDIS DMA buffers are now allocated at bind time, not here) |
| long + 2 | inside step 5: DosOpen of PROTMAN$ returned |
| 5 | NDIS MAC registered with the Protocol Manager |
| 6 | PROTOCOL.INI read |
| 7 | INIT finished |
| 8 / 9 | INIT COMPLETE entered / finished |

The hang is in the code that follows the last step you heard (for example 3 beeps and then silence: the BAR0
mapping). No beep at all means the driver never reached its INIT (a problem with loading the module itself).
The same step numbers are written to COM1 as `[S3]`. The beeps need a working PC speaker; some laptops mute it.

**Stop early.** `/S1` ... `/S9` end INIT successfully right after that step and skip the rest, so the boot
continues: `DEVICE=C:\IBMCOM\MACS\QCA6174.OS2 /S3` does the first three steps only. The smallest n for which the
boot hangs again shows the step that hangs (`/S1` should boot; if even that hangs, it is the module or its
header). Combine with `/B` to hear the steps.

Result of the first hang report: 4 beeps and nothing after the first sub-step marker, so the hang was in the first
write to DMA memory through a GDT selector that had been mapped during INIT. INIT no longer maps or touches DMA
memory or BAR0: the selectors are allocated at INIT (that is only allowed there) but mapped on first use at task
time, and the NDIS buffers are allocated when the protocol binds.

## 9. First contact with the hardware, with no serial port

The status text has a line `DBG=n ADDR=a VAL=v BARWIN=w`: the last debug read. `echo r <hex offset> > \DEV\WRND32$`
maps BAR0 (on the first use) and reads one 32-bit register; `echo d <hex target address>` reads target memory
through the diagnostic copy engine (only after a successful `up`). Read the result with `copy \DEV\WRND32$ con`.

1. `echo r 80000 > \DEV\WRND32$`, then the status: `BARWIN=20` (32 windows mapped) and `VAL` anything but
   `FFFFFFFF` means the registers are reachable. (Offset 80000 is PCIE_LOCAL + RTC_STATE; bits 2:0 are 3 when the
   chip is awake.) `VAL=FFFFFFFF` means the mapping or the device does not answer.
2. `echo r 3a028 > \DEV\WRND32$` reads the firmware indicator register.
3. Only then the upload and `echo up`. `echo up` is the first thing that resets the chip; if the machine freezes
   there, the last `STATE=` cannot be read, so take the steps above first and note which one was the last to work.

## 10. A scan finds no 2.4 GHz networks

The status text has scan statistics: `SCANCH=` is a 48-bit map (hex, channel 1 = the lowest bit) of the channels
the firmware reported visiting, in the order of the driver's channel list (1-13, then 36 ... 165); `SCEV`/`LASTTYPE`
count scan events and show the last type (`0002` = completed); `MG2G=` is one byte per 2.4 GHz channel 1-14 counting
the management frames received there, `MG5G` the 5 GHz total, `MGDROP`/`ST` frames dropped for a non-zero receive
status. If `SCANCH` shows the 2.4 GHz bits set but `MG2G` stays 0, the firmware visited the channels and heard nothing
(antenna, regulatory domain or genuinely no networks); if the bits are clear, the channel list or the regulatory
domain keeps the firmware from scanning them.

## 11. The Wireless LAN Monitor does not see the adapter

The monitor finds a GenMAC adapter through `PROTOCOL.INI` and the NIF file, then talks to `\DEV\WRND32$`. Check, in order:

1. `C:\IBMCOM\MACS\QCA6174.NIF` exists (or the adapter was installed through Network Adapters and Support).
2. `C:\IBMCOM\PROTOCOL.INI` has `QCA6174_NIF = QCA6174.NIF` in `[IBMLXCFG]`, a `[QCA6174_NIF]` section with
   `DriverName = WRND32$`, and `QCA6174_NIF` in the `Bindings` line of `[TCPIP_NIF]`. The adapter name the monitor
   and the supplicant use is `lanN`, N = the position in that `Bindings` list counting from 0 (`LANNUM=` in the
   status text is what the driver derived from the file).
3. The driver is loaded (`copy \DEV\WRND32$ con` works) and no other driver uses `WRND32$`.
4. Start the monitor, then read the status text. `IOCTLS=` counts every IOCtl that reached the driver and `OTHER=` /
   `LASTOTHER` those that were not GenMAC requests. Below them the last 16 requests are listed, oldest first, as
   `GET|SET <oid> -> <NDIS status>`: `00000000` is success, `C00000BB` not supported, `C0010011` adapter not ready,
   `C0010016` buffer too short. `IOCTLS=0000` means the monitor never reached the driver (items 1-3, or the
   monitor did not find the adapter in `PROTOCOL.INI`); a request answered `C00000BB` is an OID the monitor needs
   and the driver does not provide.

## 12. NETADDRESS (IP over NDIS)

TCP/IP copies the adapter MAC when it binds at boot, before the firmware is up. Without
`NETADDRESS = "xxxxxxxxxxxx"` under `[QCA6174_NIF]` in PROTOCOL.INI the stack uses 00:00:00:00:00:00 and
neither ARP nor ping work. The status text shows `MAC=` (in use) and `FWMAC=` (chip); use the `FWMAC` value.

The adapter now starts automatically when the second firmware file is copied. If the firmware copy runs
before the network is bound (STARTUP.CMD before NETBIND), `NETADDRESS` is no longer needed; the status text
says `(firmware)` next to `MAC=` and `ndis_set_mac` hands the chip's address to the stack. If the stack still
binds first, keep `NETADDRESS`.

### NETADDRESS helper

The stack binds before STARTUP.CMD can load the firmware, so the real MAC cannot reach it at bind time
(the driver can't touch the card during boot). Run `install\QCMAC.CMD` once after the adapter is READY: it reads
`FWMAC=` from the status text and writes `NETADDRESS` into `[QCA6174_NIF]` (a `.bak` copy is made). Reboot afterwards.

## 13. HT20 (speed)

If the AP's beacon has an HT capabilities element, the driver joins in HT20: the vdev channel mode is HT20,
the association request carries an HT capabilities element (HT20, SGI20, 2 streams MCS 0-15 receive mask byte 0/1),
the peer is associated with the AP's HT caps/MCS set (flags HT|QOS), and data frames use TID 0 (native-wifi frames carry no QoS control field; the firmware adds it).
Status text: `HT=on QOS=01 APMCS=FFFF NSS=02`. No A-MPDU receive agreement (ADDBA is not answered) and no 40/80 MHz
or VHT yet; expect roughly 3-6x the legacy-rate speed. If the join fails or traffic stops, report the status text:
the quick way back is a build before this commit.

## 14. 40 / 80 MHz channels

The scan table now keeps, per beacon, the AP's HT capabilities (40 MHz bit), HT operation (secondary channel offset,
STA channel width) and VHT operation (channel width, centre segment index). At join:

* 5 GHz, AP on a 40 MHz channel: HT40 (vdev mode 6, `HT40_PLUS` when the secondary channel is above, centre = primary +/- 10 MHz).
* 5 GHz, AP on 80 MHz (VHT operation width >= 1, centre index inside 5170-5835): VHT80 (mode 10, `band_center_freq1` =
  5000 + 5 x index). The association request then carries a VHT capabilities element (MCS 0-9, 1 or 2 streams from `RFCHAINS`,
  no 160 MHz); the peer is associated with the AP's VHT caps and MCS map (flags VHT|80MHZ, phymode 10).
* 2.4 GHz: HT40 only when the AP's HT capabilities and operation say so.

Status text: `WIDTH=80 MAX=2 SCO=1 CC0=..` (hex centre index), then `HT=on ...`.

If the join fails or traffic stops with the wider channel, set `MAXWIDTH = 20` (or `40`) under `[QCA6174_NIF]` in
PROTOCOL.INI, reboot, and send the status text of both runs. A VHT station must receive 80 MHz frames, so 80 MHz is
never advertised without the vdev actually running on the 80 MHz channel.

### Pinning the channel / width for comparisons

`[QCA6174_NIF]` in PROTOCOL.INI: `CHANNEL = 136` joins only an access point whose primary channel is 136 (the status
text shows `FORCECH=88`, hex); `MAXWIDTH = 20|40|80` limits the width. Use both to compare widths on the same AP.

### Duplicate frames on one access point (DEDUP)

On one AP (channel 149, EAP770) 63% of the TCP data arrived twice: identical IP header (same IP id) and TCP sequence,
different 802.11 sequence numbers (RXTRACE in the status text), so the AP or the network behind it sends the copies.
`DEDUP = 1` under `[QCA6174_NIF]` makes the driver drop an IPv4 frame identical to one of the last 64 delivered (header +
first 8 payload bytes; a real TCP retransmission has a new IP id and passes). The status text shows `DEDUP=1 dropped=...`.
It is a workaround: the cause (AP or wired side) still needs finding, e.g. a capture on the server during a download.
