# SPDX-License-Identifier: GPL-2.0-only
# Cross build on Linux with Open Watcom 2.0 (binl64 on PATH, WATCOM set) and NASM:  make
# On OS/2 use build.cmd (same flags).
CFLAGS = -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j
ASM    = qc_hdr qc_lib qc_ndisx qc_end
CSRC   = qc_strat qc_pci qc_mem qc_fw ath_ce ath_boot ath_htc ath_wmi ath_sta qc_ndis qc_nic qc_dbg
OBJS   = $(addsuffix .obj,$(ASM) $(CSRC))
HDRS   = qca.h qc_os2dd.h qc_ndis.h

QCA6174.OS2: $(OBJS) qca6174.lnk
	wlink @qca6174.lnk

%.obj: %.asm
	nasm -f obj $< -o $@

%.obj: %.c $(HDRS)
	wcc $(CFLAGS) -fo=$@ $<

clean:
	rm -f *.obj *.err QCA6174.OS2 QCA6174.map

.PHONY: clean
