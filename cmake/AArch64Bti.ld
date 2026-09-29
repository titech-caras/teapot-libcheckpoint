/* Explicit page isolation, valid for Linux 4/16/64 KiB pages.
   Normal text and the speculative copy share one guarded window: the copy starts
   on its own 64 KiB boundary right after normal text, and the enforcement probes
   follow it. The __teapot_bti_* bounds come from the rewriter's aliases (whole
   program) or from the test layout object. */
SECTIONS
{
  .teapot_bti_normal ALIGN(65536) :
  {
    __teapot_bti_guard_start = .;
    KEEP(*(.teapot_bti_normal))
    . = ALIGN(65536);
    KEEP(*(.teapot_transient))
    . = ALIGN(16);
    KEEP(*(.teapot_bti_probe))
    . = ALIGN(65536);
    __teapot_bti_guard_end = .;
  }
}
INSERT AFTER .text;
