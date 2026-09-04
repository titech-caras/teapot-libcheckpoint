teapot_dift_layout(x64-la48
    ARCH x64
    XOR_MASK 0x200000000000
    ASAN_SHADOW_OFFSET 0x7fff8000
    APP_RANGES
        0x0:0x7fff8000
        0x600000000000:0x800000000000
    DEFAULT_FOR x64)

teapot_dift_layout(x64-la48-asan-new
    ARCH x64
    XOR_MASK 0x300000000000
    ASAN_SHADOW_OFFSET 0x7fff8000
    APP_RANGES
        0x0:0x7fff8000
        0x7fff8000:0x8fff7000
        0x2008fff7000:0x10007fff8000
        # GCC 14's ASan primary allocator occupies the 0x50... region.  Its
        # DIFT shadow therefore belongs in the disjoint 0x60... region.
        0x500000000000:0x600000000000
        # Shared objects and the process stack normally occupy 0x70....
        0x700000000000:0x800000000000)

teapot_dift_layout(x64-la57
    ARCH x64
    XOR_MASK 0x40000000000000
    ASAN_SHADOW_OFFSET 0x7fff8000
    APP_RANGES
        0x0:0x40000000000000)

teapot_dift_layout(aarch64-vma39
    ARCH aarch64
    XOR_MASK 0x2000000000
    ASAN_SHADOW_OFFSET 0x1000000000
    APP_RANGES
        0x0:0x1000000000
        0x1000000000:0x1200000000
        0x1400000000:0x2000000000
        # QEMU user-mode and native top-down allocators may place large
        # anonymous mappings below the loader and stack.  Cover the top 4 GiB
        # instead of assuming every high mapping fits in the last 256 MiB.
        0x7f00000000:0x8000000000)

teapot_dift_layout(aarch64-vma42
    ARCH aarch64
    XOR_MASK 0x20000000000
    ASAN_SHADOW_OFFSET 0x1000000000
    APP_RANGES
        0x0:0x1000000000
        0x5000000000:0x20000000000
        0x3fff0000000:0x40000000000
    DEFAULT_FOR aarch64)

teapot_dift_layout(aarch64-vma48
    ARCH aarch64
    XOR_MASK 0x400000000000
    ASAN_SHADOW_OFFSET 0x1000000000
    APP_RANGES
        0x0:0x400000000000
        0xfffff0000000:0x1000000000000)

teapot_dift_layout(riscv64-sv39
    ARCH riscv64
    XOR_MASK 0x2000000000
    ASAN_SHADOW_OFFSET 0xd55550000
    APP_RANGES
        0x0:0xd55550000
        0xd55550000:0xeffffa000
        0xfffffa000:0x1555550000
        0x1555550000:0x2000000000
        0x3ff0000000:0x4000000000)

teapot_dift_layout(riscv64-sv48
    ARCH riscv64
    XOR_MASK 0x200000000000
    ASAN_SHADOW_OFFSET 0xd55550000
    APP_RANGES
        0x0:0xd55550000
        0x4000000000:0x6000000000
        0x7ffff0000000:0x800000000000
    DEFAULT_FOR riscv64)

teapot_dift_layout(riscv64-sv57
    ARCH riscv64
    XOR_MASK 0x40000000000000
    ASAN_SHADOW_OFFSET 0xd55550000
    APP_RANGES
        0x0:0xd55550000
        0x4000000000:0x6000000000
        0xffffff00000000:0x100000000000000)
