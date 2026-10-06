# Generated v4 executable fixtures

These exact ordinary-printer/prepared outputs contain two whole-instruction
sites: destination bootstrap followed by original-input dead scratch. The RV
fixture starts with the input's existing compressed NOP, placing the sites at
halfword addresses; no alignment NOP was added. All relocation-bearing RV stub
instructions are inside the owned norvc/norelax scope, and the dedicated object
target alone uses `-Wa,--no-pad-sections`.

The retained reviewed l6 outputs had these SHA-256 identities:

- AArch64: `6b0f7ab46f90e9950674a3795f737a55df54950333feda39f556771e7c1c45de`
- RV64: `8b5021c168487283bce94c2abbc1860abb37be08ea80fadc95474117af1240db`

Teapot's `tests/test_fault_risc_integration.py` recreates their GTIRB inputs,
runs the actual emitter and printer/preparation, and assembles both sources
after canonicalizing only generated UUIDs. It compares section bytes, sizes,
alignment and flags, every retained symbol's position/binding/type/visibility,
and relocation offsets/types/addends/targets. Only symbol-table ordering and
the assembly file name are ignored: the printer can reorder co-located aliases.
An encoder/emitter change must update the fixture intentionally and pass both
Python and runtime tests. Cross compilers are required for this Python gate.

CMake builds these checked-in files without requiring Teapot or a printer at
runtime-library build time. Publishing-enabled RISC CTest lanes run execution
ON/OFF, A64's manual-policy PAC/BTI variant in the configured BTI lane and, when checkpoint_nested is built,
eight real rollback combinations with exact ON/OFF output comparison. The QEMU
A64 rollback fixture uses its retained 16-MiB guest stack argument. These tests
do not establish native cache/migration, full backend BTI activation or full ASan.
The rollback fixture honors MTE's existing no-ASan-poison policy and manually
initializes/protects its copy bounds in the configured BTI lane. Those fixture
adaptations do not change production runtime policy or forwarding behavior.
