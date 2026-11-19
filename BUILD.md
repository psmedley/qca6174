# Building

Open Watcom C 2.0 (16-bit `wcc`, `wlink`) and NASM. Nothing from the IBM DDK.

## Linux (cross build)

```sh
export WATCOM=/path/to/open-watcom-2
export PATH=$WATCOM/binl64:$WATCOM/binl:$PATH
make -C src
```

The result is `src/QCA6174.OS2` (a 16-bit NE module, like the RTL drivers); `src/QCA6174.map` is the link
map. `make -C src clean` removes the objects.

## OS/2 / ArcaOS

Open Watcom `binp` and NASM on the `PATH`, `WATCOM` set, `INCLUDE` empty; in `src` run `build.cmd`.

## Flags and rules

`wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j`

* `-2`: 80286 code. 32-bit registers are only used in `qc_lib.asm`, which saves their upper halves.
* `-ms -zu`: small model, SS is not DS. **Never pass the address of a stack variable** to a function that
  takes a near pointer: the compiler warns "W112 Pointer truncated" - treat every such warning as a bug
  (make such variables `static`).
* `UCHAR FAR *a, *b;` declares `b` as a *near* pointer. Write `UCHAR FAR *a, FAR *b;`.
* No C runtime: no 32-bit multiply, divide or variable shift (the link would fail with an undefined helper).
* `qc_hdr.obj` must be linked first (device header at offset 0), `qc_end.obj` last (resident-end markers).
* DGROUP (header, data, BSS) must stay below 64 KB; it is about 17 KB. Large buffers live in DMA memory.
