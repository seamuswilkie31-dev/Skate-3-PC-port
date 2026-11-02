// Call counters on the functions that write GPU command packets, for mapping Skate 3's
// Direct3D library (0x82B4xxxx-0x82B6xxxx) and the game code that writes packets
// itself. Found by scanning the recompiled code for packet headers; the labels say
// which packets each one builds, or how it was anchored. See docs/D3D.md.

#include "hooks/hooks.h"

// Anchored through the kernel functions they call.
S3N_COUNT_CALLS(82B5A648, "swap (calls VdSwap)")
S3N_COUNT_CALLS(82B5DB18, "engine init (VdInitializeEngines)")
S3N_COUNT_CALLS(82B4D390, "ring buffer init")
S3N_COUNT_CALLS(82B4C5A8, "kick-off (writes CP_RB_WPTR)")
S3N_COUNT_CALLS(82B4CEF8, "insert callback (INTERRUPT)")
S3N_COUNT_CALLS(82B4C9B0, "fence (EVENT_WRITE_SHD)")

// Direct3D functions that emit draws.
S3N_COUNT_CALLS(82B4D998, "draw: DRAW_INDX, IM_LOAD_IMM")
S3N_COUNT_CALLS(82B51800, "draw: DRAW_INDX, EVENT_EXT #1")
S3N_COUNT_CALLS(82B51C98, "draw: DRAW_INDX, EVENT_EXT #2")
S3N_COUNT_CALLS(82B521B0, "draw: DRAW_INDX, EVENT_EXT #3")
S3N_COUNT_CALLS(82B525A8, "draw: DRAW_INDX, EVENT_EXT #4")
S3N_COUNT_CALLS(82B533B0, "draw: DRAW_INDX_2")
S3N_COUNT_CALLS(82B53508, "draw: DRAW_INDX/_2 #1")
S3N_COUNT_CALLS(82B53D40, "draw: DRAW_INDX/_2 #2")
S3N_COUNT_CALLS(82B54830, "draw: DRAW_INDX, IM_LOAD_IMM")
S3N_COUNT_CALLS(82B55668, "draw: many packets")
S3N_COUNT_CALLS(82B564D8, "draw: DRAW_INDX #5")
S3N_COUNT_CALLS(82B567C8, "draw: DRAW_INDX_2 #2")
S3N_COUNT_CALLS(82B629B8, "draw: DRAW_INDX #6")
S3N_COUNT_CALLS(82B62B10, "draw: DRAW_INDX/_2 #3")
S3N_COUNT_CALLS(82B633C8, "draw: DRAW_INDX_2 #3")
S3N_COUNT_CALLS(82B68D18, "draw: DRAW_INDX/_2 #4")
S3N_COUNT_CALLS(82BB9410, "draw: DRAW_INDX #7")

// Shader and constant uploads inside the library.
S3N_COUNT_CALLS(82B5C640, "shader: IM_LOAD, SET_CONSTANT")
S3N_COUNT_CALLS(82B5C3C0, "constants: LOAD_ALU_CONSTANT")
S3N_COUNT_CALLS(82B5C260, "shader: IM_LOAD_IMM")
S3N_COUNT_CALLS(82B4DF10, "constants: SET_CONSTANT #1")
S3N_COUNT_CALLS(82B5B0F0, "constants: SET_CONSTANT #2")
S3N_COUNT_CALLS(82B627B8, "constants: SET_CONSTANT #3")
S3N_COUNT_CALLS(82B56F08, "indirect buffer #1")

// Game code (outside the library) that writes packets itself.
S3N_COUNT_CALLS(827BD490, "game: DRAW_INDX_2 #1")
S3N_COUNT_CALLS(82986420, "game: DRAW_INDX_2 #2")
S3N_COUNT_CALLS(82C7AA28, "game: DRAW_INDX_2 #3")
S3N_COUNT_CALLS(829ADB38, "game: SET_CONSTANT #1")
S3N_COUNT_CALLS(829B0268, "game: SET_CONSTANT #2")
S3N_COUNT_CALLS(829DC388, "game: SET_CONSTANT #3")
S3N_COUNT_CALLS(829BED58, "game: IM_LOAD_IMM #1")
S3N_COUNT_CALLS(829D80B8, "game: IM_LOAD_IMM #2")
S3N_COUNT_CALLS(829DFE28, "game: IM_LOAD_IMM #3")
S3N_COUNT_CALLS(829D6378, "game: IM_LOAD")
S3N_COUNT_CALLS(825BE8D0, "game: LOAD_ALU_CONSTANT")
S3N_COUNT_CALLS(82BAEB88, "game: indirect buffer #1")
S3N_COUNT_CALLS(82BAFA00, "game: indirect buffer #2")
S3N_COUNT_CALLS(82BB0240, "game: indirect buffer #3")
S3N_COUNT_CALLS(82D0CDF0, "game: IM_LOAD_IMM #4")
S3N_COUNT_CALLS(82D0D910, "game: IM_LOAD_IMM #5")
