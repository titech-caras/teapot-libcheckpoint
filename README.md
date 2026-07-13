# libcheckpoint

This repository contains the Teapot runtime library.  It builds x86-64,
AArch64, and RV64 variants.

## Layout

- `include/`: public runtime headers and architecture constants.
- `src/`: C runtime implementation.
- `src/dift_wrappers/`: DIFT external-call wrappers.
- `asm/`: architecture-specific checkpoint/restore assembly.

## Build

Build the runtime with CMake and select the target architecture from the
compiler target, `CMAKE_SYSTEM_PROCESSOR`, or `-DCHECKPOINT_ARCH=...`.

```shell
cmake -S libcheckpoint -B build-libcheckpoint \
  -DCHECKPOINT_ARCH=riscv64 \
  -DTEAPOT_DIFT_LAYOUT=riscv64-sv39
cmake --build build-libcheckpoint
```

The runtime DIFT layout must match the Teapot instrumentation
`--dift-layout`.  Useful profiles include `x64-la48`, `x64-la48-asan-new`,
`aarch64-vma39`, `aarch64-vma42`, `riscv64-sv39`, and `riscv64-sv48`.
The layout profiles are defined in `cmake/DiftLayoutData.cmake`; the Teapot
Python instrumentation reads that same file.

The default `checkpoint` target fixes the runtime depth to one checkpoint.  Pass
`-DTEAPOT_BUILD_NESTED_RUNTIME=ON` to also build the `checkpoint_nested` target,
then link that target with Teapot output produced with
`--enable-nested-speculation`.

Shadow-tag instrumented binaries must link ASan.  Libcheckpoint expects ASan
shadow memory to exist; there is no fallback ASan shadow mapper.  Runtime
metadata lives in the `teapot_protected` section, which libcheckpoint poisons
through ASan in shadow-tag mode.  Keep direct runtime accesses in libcheckpoint
code; do not use intercepted libc memory routines such as `memcpy` on protected
metadata.
For coverage builds, link a library that provides the Sanitizer Coverage
interface, such as `libhfuzz`.

AArch64 can optionally store Teapot ASan-style tags in MTE allocation tags:

```shell
cmake -S libcheckpoint -B build-libcheckpoint \
  -DCHECKPOINT_ARCH=aarch64 \
  -DTEAPOT_DIFT_LAYOUT=aarch64-vma42 \
  -DTEAPOT_AARCH64_TAG_STORAGE=mte
```

This must match Teapot instrumentation produced with
`--aarch64-tag-storage=mte`.  MTE is used only as compact metadata storage:
libcheckpoint enables tagged addresses with no tag-check fault mode, and Teapot
instrumentation explicitly loads allocation tags and branches in software.
MTE stores four tag bits per 16-byte granule; AArch64 MTE mode treats matching
logical and allocation tags as unpoisoned, and mismatches as poisoned.  At
startup, libcheckpoint applies
`PROT_MTE` to writable mappings that overlap the selected DIFT app ranges, but
does not sweep the whole address space writing zero tags.  MTE mode poisons the
protected runtime metadata with allocation tags and should not link ASan unless
another experiment explicitly needs it.
The provided Teapot Docker image also contains an MTE-capable arm64 glibc
sysroot at `/opt/aarch64-mte-sysroot`.  Its malloc MTE support can be enabled
with `GLIBC_TUNABLES=glibc.mem.tagging=1`.

## Optional Wrappers

The core runtime includes the libc DIFT wrappers used by existing tests.  The
wrappers that require extra libraries can be built as opt-in companion targets:

- `checkpoint_dift_math_wrappers`: wrappers that require `libm`.
- `checkpoint_dift_zlib_wrappers`: wrappers that require zlib.

Only link these libraries when the instrumented binary references the matching
`__dift_wrapper__` symbols.

## Architecture Notes

RV64 does not assume floating-point registers are available.  Build with
`-DTEAPOT_ENABLE_RISCV_FLOAT_STATE=ON` only for targets that provide the
floating-point state Teapot should checkpoint.

For RISC-V Sv39 qemu user-mode smoke tests, reserve the low user virtual address
space:

```shell
qemu-riscv64 -R 0x4000000000 -L /usr/riscv64-linux-gnu ./a.inst input.txt
```

For AArch64 qemu user-mode smoke tests, use `aarch64-vma42` for MTE mode.  The
smaller `aarch64-vma39` profile can place DIFT shadow memory where the dynamic
loader or stack lives under qemu user-mode.

```shell
qemu-aarch64-mte -cpu max -R 0x40000000000 -s 33554432 -L /opt/aarch64-mte-sysroot ./a.inst input.txt
```

The AArch64 first-spill fallback uses fixed shadow-stack frames below the
application stack.  Instrumentation passes that can nest must use distinct
frame offsets and scratchpad save windows.
