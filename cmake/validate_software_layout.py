#!/usr/bin/env python3
"""Whole-program layout validation for the software-mode window (decision 6).

The instrumentation checks one range, ``[.__text_start, .__transient_end)``,
plus the marker pair, so the application's normal text must sit immediately
before the speculative copy with no other allocated section between them, and
the runtime's text, the trampolines, the PLT and ``.init``/``.fini`` must stay
outside.

Link a whole-program binary with the per-ISA script that ships next to this
file (``X64Software.ld``, ``Riscv64Software.ld`` or ``AArch64Software.ld``),
then run this check on the linked binary:

    python3 validate_software_layout.py a.out

Exit status 0 and a JSON object on stdout mean the window is exact.  Requires
pyelftools.
"""
import json
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

BOUNDS = {
    'text_start': '.__text_start__teapot__',
    'text_end': '.__text_end__teapot__',
    'transient_start': '.__transient_start__teapot__',
    'transient_end': '.__transient_end__teapot__',
}
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
# Named so a failure says which contract broke, not just "a section".
OUTSIDE = ('.text', '.teapot_trampolines', '.init', '.fini', '.plt')


def validate(binary):
    with open(binary, 'rb') as stream:
        elf = ELFFile(stream)
        symbols = {}
        for section in elf.iter_sections():
            if isinstance(section, SymbolTableSection):
                for symbol in section.iter_symbols():
                    if symbol.name in BOUNDS.values():
                        symbols[symbol.name] = symbol['st_value']
        missing = sorted(name for name in BOUNDS.values() if name not in symbols)
        if missing:
            raise ValueError('missing window bounds: ' + ', '.join(missing))
        text_start = symbols[BOUNDS['text_start']]
        text_end = symbols[BOUNDS['text_end']]
        transient_start = symbols[BOUNDS['transient_start']]
        transient_end = symbols[BOUNDS['transient_end']]
        if not 0 < text_start < text_end <= transient_start < transient_end:
            raise ValueError('window bounds are not ordered: '
                             '%x %x %x %x' % (text_start, text_end, transient_start, transient_end))

        normal = elf.get_section_by_name('.teapot_normal')
        if normal is None:
            raise ValueError('the link has no .teapot_normal output section')
        # The rewriter may insert an entry marker before .__text_start (the
        # window bound), so the section may begin a little earlier; it must
        # contain the whole window.
        if normal['sh_addr'] > text_start or normal['sh_addr'] + normal['sh_size'] < transient_end:
            raise ValueError('.teapot_normal does not contain the window: %x+%x vs %x..%x'
                             % (normal['sh_addr'], normal['sh_size'], text_start, transient_end))
        if not normal['sh_flags'] & SHF_EXECINSTR:
            raise ValueError('.teapot_normal is not executable')

        for section in elf.iter_sections():
            if section.name == '.teapot_normal' or not section['sh_flags'] & SHF_ALLOC:
                continue
            if section['sh_size'] == 0:
                continue
            start = section['sh_addr']
            end = start + section['sh_size']
            if end > text_start and start < transient_end:
                raise ValueError('allocated section %s (%x..%x) lies inside the window %x..%x'
                                 % (section.name, start, end, text_start, transient_end))
        for name in OUTSIDE:
            section = elf.get_section_by_name(name)
            if section is None:
                continue  # -nostartfiles links may have no .init/.fini/.plt
            start = section['sh_addr']
            end = start + section['sh_size']
            if end > text_start and start < transient_end:
                raise ValueError('%s lies inside the window' % name)
        return {'text_start': text_start, 'text_end': text_end,
                'transient_start': transient_start, 'transient_end': transient_end,
                'window': transient_end - text_start,
                'normal': normal['sh_size']}


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    try:
        result = validate(sys.argv[1])
    except ValueError as error:
        print('invalid software window: %s' % error, file=sys.stderr)
        return 1
    print(json.dumps(result, indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
