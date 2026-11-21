/* nwgba - the whole core as one piece, so that the compiler sees it all at once
 * (and so that there is a single set of counts for tools/pgo.sh). */
#include "gba.c"
#include "cpu.c"
#include "mem.c"
#include "ppu.c"
#include "bios.c"
#include "pack.c"
#include "save.c"
#include "irqm.c"
