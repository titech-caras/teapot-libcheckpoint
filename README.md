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
Single-configuration builds default to `Release` (`-O3`); explicit `Debug`
builds remain available. Restore routines use the selected optimization level,
including fault-recovery paths, without per-function `-O0` overrides.

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
Both configuration paths reject overlapping application/DIFT regions and DIFT
regions that intersect ASan shadow. Application ranges include some ASan-backed
addresses because instrumentation can also propagate tags for those accesses.
These static checks do not guarantee a particular loader placement is supported;
startup still rejects reservations that collide with existing mappings.

The default `checkpoint` target fixes the runtime depth to one checkpoint.  Pass
`-DTEAPOT_BUILD_NESTED_RUNTIME=ON` to also build the `checkpoint_nested` target,
then link that target with Teapot output produced with
`--enable-nested-speculation`.
The nested branch-count heuristic is capped at `MAX_CHECKPOINTS`.

### Software-mode layout

Software mode needs no special layout. Its indirect-target check in the
speculative copy tests only the marker pair at the target, for branches, calls
and returns alike, so an ordinary link works. Uninstrumented code carries no
pair and rolls back; a target whose pair load faults rolls back through the
signal handler. The `aarch64-bti-pac` target keeps its guarded-copy script,
`cmake/AArch64Bti.ld`.

With `BUILD_TESTING=ON`, CTest includes signal tests and real checkpoint-entry
probes for capacity, counter rollover, timing classification, and memory-history
recovery after a read-only destination faults. Entry probes
build the runtime sources with `TIME` in both default and nested modes; this
does not enable those options in the ordinary `checkpoint` target.
On AArch64, an additional entry probe checks actual `PROT_BTI` fault recovery
and original-handler forwarding. It verifies memory history, DIFT tags, selected
GPRs and the instruction counter; missing hardware/OS enforcement is an explicit
CTest skip. The nested variant creates an inner checkpoint at an initialized
depth-one state, not a live outer checkpoint chain. Recovery code remains
unguarded, so this is not validation of a BTI target-identification backend.
Cross-compiled tests need a suitable `CMAKE_CROSSCOMPILING_EMULATOR` launcher,
including the address-space reservation required by QEMU.
The single-threaded runtime reserves a guarded alternate signal stack with the
kernel-reported signal-frame minimum plus 64 KiB of handler space (at least
`SIGSTKSZ` is reserved for the frame on older kernels). This avoids overflowing
old libc's fixed `SIGSTKSZ` during out-of-simulation diagnostics and forwarding.
ASan-only tests are registered only when a real executable link probe succeeds,
including in cross builds. Static-only configurations omit those tests explicitly;
they still run the non-ASan runtime tests, not a substitute shadow mapper.
Report-suppression tests cover page boundaries and permission failures, including
simulated 64 KiB pages. Suppression uses the runtime page size and `/proc/self/maps`
to restore the affected pages' original read/write/execute permissions. Failure to
restore those permissions terminates the process.
Reporting uses bounded protected storage and syscall I/O, without heap-backed
streams or libc formatting. ASan's `snprintf` interceptor rejects poisoned output
buffers. Tests cover the full report format, poisoned storage, partial/interrupted
I/O and long maps records. Unreadable permissions leave the call unchanged with
a warning; permissions are never guessed. The repaired AArch64 MTE runtime passes
all 120 libhtp test_fuzz inputs under QEMU; native MTE is a separate gate.
The x64 report-state test links ASan and poisons the runtime scratch stack, so
libc output buffers accidentally placed there are caught by the test.
For GCC 13+ ASan, configure `-DTEAPOT_DIFT_LAYOUT=x64-la48-asan-new` for this
test. CTest disables it for GCC 13+ with the legacy `x64-la48` layout, which can
abort during preinitialization; a disabled test is not validation. Under a tracing
launcher, run CTest with `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`.
Libc wrapper tests compare return values, failure behavior, destination bytes,
and tag-update ranges, including fortified-call bounds and string terminators.
Block-input tests also cover partial `fread` elements, pipes and stream errors;
the wrappers tag the copied tail without changing the returned item count.
Line-input tests cover embedded NULs, carriage returns/newlines, EOF, partial
errors and preexisting stream flags. The `fgets` wrapper uses glibc's exported
`_IO_getline` count, since scanning the result cannot recover these write bounds.
It preserves [glibc's fgets error/termination contract](https://github.com/bminor/glibc/blob/master/libio/iofgets.c),
tags copied input bytes even on partial failure, and clears only an actually
written terminator. This wrapper requires glibc, not an interposed replacement
with different semantics; it does not model `__fgets_chk`.
The optional zlib wrappers require zlib 1.2.6 or later. `inflate` tags only the
produced output interval, using a conservative union of consumed-input tags
retained across calls. This is not exact compressed-byte provenance. Init/reset,
copy/end, dictionary, prime and sync calls maintain that state; ResetKeep retains
history. Output cursors retain their own pointer dependencies. Checksum tags are
conservative across resets because raw mode can leave `adler` unchanged.
All stream lifecycle calls must reach the wrappers: calls hidden inside other
uninstrumented libraries are not intercepted. A missing stream record warns
and uses all tag bits; metadata allocation failure aborts rather than losing
provenance silently. The runtime remains single-threaded. `inflateBack` and
`inflateGetHeader`/`inflateGetDictionary` output effects are not modeled.
The zlib tests compare actual output/status against unwrapped zlib and check
byte tags, unused tails, buffered output, stream lifetimes, dictionaries, partial
errors and protected-page boundaries on each supported ISA.

Shadow-tag instrumented binaries must link ASan.  Libcheckpoint expects ASan
shadow memory to exist; there is no fallback ASan shadow mapper.  Runtime
metadata lives in `teapot_protected` and `teapot_protected_bss`, both poisoned
through ASan in shadow-tag mode. The shared `asm/storage.S` reserves the 29 MiB
memory history, guard list and scratchpad as NOBITS; initialized pointers stay
in C's data section. This reduces file size, not the virtual reservation, and
requires no linker script. MTE mode leaves runtime metadata untagged.
Protected-section declarations use `LIBCHECKPOINT_ASSERT_PROTECTED` to check
whole 8-byte granules and preserve stronger type alignment, such as XSAVE's
64 bytes. Byte arrays need explicit alignment. The runtime also validates section
bounds rather than extending poison into neighboring data.
Keep direct runtime accesses in libcheckpoint code; do not use intercepted libc
memory routines such as `memcpy` on protected metadata.
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
startup, libcheckpoint applies `PROT_MTE` to anonymous writable mappings within
the selected DIFT app ranges, without sweeping them to write zero tags. Ordinary
file-backed globals and the protected runtime section remain untagged; unlike
shadow mode, MTE mode does not detect accesses to runtime metadata through
poison tags. Do not link ASan in MTE mode unless another experiment needs it.
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

AArch64 and RV64 checkpoint sites reserve `x16`/`x17` and `t0`/`t1`,
respectively.  Teapot keeps their values in up to two LRA-selected spare
registers and first-spills only the remainder.  The runtime uses the source
offsets in `checkpoint_target_metadata` to repair the canonical checkpoint
slots, so Teapot and libcheckpoint revisions must keep this entry convention
in sync.

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
Size and slot constants are shared in `include/aarch64_shadow_stack.h` (8 MiB
by default). To use an alternate header, select the same file at both stages:

```shell
cmake -S libcheckpoint -B build-libcheckpoint \
  -DTEAPOT_AARCH64_SHADOW_STACK_CONFIG=/absolute/path/shadow_stack.h
TEAPOT_AARCH64_SHADOW_STACK_CONFIG=/absolute/path/shadow_stack.h teapot input.gtirb output.gtirb
```

`cmake --install` installs the selected header in `include/` and the shared
DIFT profiles in `share/libcheckpoint/DiftLayoutData.cmake`. Set Python's
`TEAPOT_AARCH64_SHADOW_STACK_CONFIG` and `TEAPOT_DIFT_LAYOUT_FILE` to these files
when Teapot is installed separately. The `checkpoint-config` install component
can export just these two files; it does not build or install the runtime archive.

Keep numeric definitions decimal (no leading zeroes) and spill slots disjoint. The current
single-instruction SP adjustment supports 1-4095 multiples of 4096 bytes;
16 MiB is rejected, not silently emitted as 8 MiB. The configured size must
cover the application's maximum stack use. Independent size/control/report
CMake overrides that disagree with the shared header are rejected.
