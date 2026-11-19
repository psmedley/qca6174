@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Build QCA6174.OS2 on OS/2 / ArcaOS: Open Watcom 2.0 (binp on PATH, WATCOM set, INCLUDE empty) + NASM.
setlocal
set CFLAGS=-bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j
for %%a in (qc_hdr qc_lib qc_ndisx qc_end) do nasm -f obj %%a.asm -o %%a.obj
for %%c in (qc_strat qc_pci qc_mem qc_fw ath_ce ath_boot ath_htc ath_wmi ath_sta qc_ndis qc_nic qc_dbg) do wcc %CFLAGS% %%c.c
wlink @qca6174.lnk
endlocal
