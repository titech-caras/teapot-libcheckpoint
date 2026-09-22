/* Explicit page isolation, valid for Linux 4/16/64 KiB pages. */
SECTIONS
{
  .teapot_bti_normal ALIGN(65536) :
  {
    __teapot_bti_guard_start = .;
    KEEP(*(.teapot_bti_normal))
    . = ALIGN(16);
    KEEP(*(.teapot_bti_probe))
    . = ALIGN(65536);
    __teapot_bti_guard_end = .;
  }
}
INSERT AFTER .text;
