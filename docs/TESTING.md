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
