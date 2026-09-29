/* Software-mode window (decision 6): the application's normal text sits
   immediately before the speculative copy, and everything else -- the runtime's
   text, the trampolines, the PLT and .init/.fini -- stays outside. The bounds
   come from the rewriter's aliases (.__text_start, .__text_end,
   .__transient_start, .__transient_end), so one range test over this window is
   exact. Nothing in the generated code depends on the layout. No 64 KiB
   alignment is needed: this is the software predicate, not the BTI scan. */
SECTIONS
{
  .teapot_normal ALIGN(16) :
  {
    KEEP(*(.teapot_normal))
    . = ALIGN(16);
    KEEP(*(.teapot_transient))
  }
  .teapot_trampolines ALIGN(16) :
  {
    KEEP(*(.teapot_trampolines))
  }
}
INSERT AFTER .text;
