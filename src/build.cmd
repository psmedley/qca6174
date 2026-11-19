@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Build QCA6174.OS2 on OS/2 / ArcaOS with Open Watcom 2.0 (binp on PATH, WATCOM set) + NASM.
rem usage: build                 (compile everything and link)
rem        build file1 file2 ... (recompile only these C files, e.g. "build ath_wmi", then link)
rem The compiler flags are written out on each wcc line: CMD.EXE does not expand %VAR% the same way
rem inside a FOR command, and wcc then sees the flags as extra file names (E1139).
set WATCOM=u:\WATCOM
set PATH=u:\WATCOM\binp;C:\usr\bin;%PATH%
set INCLUDE=
set beginlibpath=u:\watcom\binp\dll
set INCLUDE=
for %%f in (qc_hdr qc_lib qc_ndisx qc_end) do nasm -f obj %%f.asm -o %%f.obj
if "%1"=="" goto all
for %%f in (%1 %2 %3 %4 %5 %6 %7 %8 %9) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j %%f.c
goto link
:all
for %%f in (qc_strat qc_pci qc_mem qc_fw ath_ce ath_boot ath_htc ath_wmi ath_sta qc_ndis qc_nic qc_dbg) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j %%f.c
:link
wlink @qca6174.lnk
