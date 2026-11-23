# Next steps (speed)

State: HT20 on 5 GHz, ~3 MB/s download (same as the RTL8821 USB driver); wired reference 15 MB/s.

## 1. Block ack - probably already done by the firmware

ath10k does not send ADDBA responses itself: the firmware answers the AP's ADDBA request and reports it to the host
with an HTT RX_ADDBA message; with `RX_FULL_REORDER` (service bit 65, set here) the firmware also reorders and the
host receives in-order indications. TX aggregation is firmware-offloaded too (ath10k denies mac80211 TX aggregation).
Evidence in the status text: about 4 frames per RX indication (`RXFRM / RXIND`) points to aggregation already working.

Check with the new `T2H=` status line (HTT message counts by type, hex): type 5 = RX_ADDBA, 6 = RX_DELBA,
0x12 = RX_IN_ORD_PADDR_IND. If type 5 is non-zero the firmware negotiates block ack on its own and nothing is needed.

## 2. Wider channels (the real next gain)

- Parse the AP's HT Operation (id 61: secondary channel offset, STA channel width) and VHT Operation (id 192: width,
  centre frequency segment 0) from the beacon (already in `net_t.ie`).
- vdev start channel: `band_center_freq1`, mode HT40 (6/7) or VHT80 (10), flags HT40_PLUS for `+` secondary.
- Association request: HT caps with the 40 MHz bit, VHT capabilities element (id 191) and operating mode; peer assoc:
  flags 40MHZ/80MHZ/VHT, `peer_vht_caps`, VHT rate set from the AP's VHT MCS map, phymode VHT80 (10).
- A VHT STA must handle 80 MHz PPDUs from the AP, so join VHT only together with the 80 MHz channel.

## 3. Other

- The 82 ms worst-case interrupt-to-task delay (`irq2hook max`); reconnect/reboot stability; 2.4 GHz with HT;
  pin the AP/band from the monitor.
