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

Instrumented binaries must link ASan.  Libcheckpoint calls `__asan_init` and
expects ASan shadow memory to exist; there is no fallback ASan shadow mapper.
Runtime metadata lives in the `teapot_protected` section, which libcheckpoint
poisons through ASan at startup.  Keep direct runtime accesses in libcheckpoint
code; do not use intercepted libc memory routines such as `memcpy` on protected
metadata.
For coverage builds, link a library that provides the Sanitizer Coverage
interface, such as `libhfuzz`.

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

For AArch64 qemu user-mode smoke tests, `aarch64-vma39` is the practical DIFT
profile.  Wider profiles are useful for larger VA experiments, but full DIFT
pre-mapping can consume substantial host memory under qemu user-mode.

```shell
qemu-aarch64 -R 0x8000000000 -s 33554432 -L /usr/aarch64-linux-gnu ./a.inst input.txt
```

The AArch64 first-spill fallback uses fixed shadow-stack frames below the
application stack.  Instrumentation passes that can nest must use distinct
frame offsets and scratchpad save windows.
