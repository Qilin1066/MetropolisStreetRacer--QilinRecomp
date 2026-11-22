// pl_mpeg's implementation (MIT, https://github.com/phoboslab/pl_mpeg; THIRD_PARTY.md).
// Its header uses size_t and FILE without including what declares them.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
