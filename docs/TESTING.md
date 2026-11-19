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
