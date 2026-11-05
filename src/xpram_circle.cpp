/*
 *  xpram_circle.cpp - XPRAM handling, bare-metal Circle specific stuff
 *
 *  Based on BasiliskII/src/Unix/xpram_unix.cpp
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"
#include "xpram.h"

// XPRAM file on the SD card
static const char XPRAM_FILE_NAME[] = "macos9rpi.nvram";


/*
 *  Load XPRAM from settings file
 */

void LoadXPRAM(const char *vmdir)
{
	FILE *f = fopen(XPRAM_FILE_NAME, "rb");
	if (f != NULL) {
		fread(XPRAM, 1, XPRAM_SIZE, f);
		fclose(f);
	}
}


/*
 *  Save XPRAM to settings file
 */

void SaveXPRAM(void)
{
	FILE *f = fopen(XPRAM_FILE_NAME, "wb");
	if (f != NULL) {
		fwrite(XPRAM, 1, XPRAM_SIZE, f);
		fclose(f);
	}
}


/*
 *  Delete PRAM file
 */

void ZapPRAM(void)
{
	remove(XPRAM_FILE_NAME);
}
