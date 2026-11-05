/*
 *  video_circle.cpp - Video/graphics emulation, bare-metal Circle specific
 *
 *  Mac OS draws directly into the Raspberry Pi's 8 bit palettized GPU
 *  framebuffer (no copying, no conversion). The palette is forwarded to the
 *  GPU through the mailbox interface. The picture goes out over HDMI.
 *
 *  Based on SheepShaver/src/Unix/video_x.cpp
 *  SheepShaver (C) 1997-2008 Marc Hellwig and Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"

#include "cpu_emulation.h"
#include "main.h"
#include "adb.h"
#include "prefs.h"
#include "user_strings.h"
#include "about_window.h"
#include "video.h"
#include "video_defs.h"
#include "rpi_glue.h"

#define DEBUG 0
#include "debug.h"


// Frame buffer
static uint8 *the_buffer = NULL;
static unsigned fb_width = 0, fb_height = 0, fb_pitch = 0;


/*
 *  Find Apple resolution ID for a screen size
 */

static int find_apple_resolution(int xsize, int ysize)
{
	if (xsize == 640 && ysize == 480)
		return APPLE_640x480;
	if (xsize == 800 && ysize == 600)
		return APPLE_800x600;
	if (xsize == 1024 && ysize == 768)
		return APPLE_1024x768;
	if (xsize == 1152 && ysize == 768)
		return APPLE_1152x768;
	if (xsize == 1152 && ysize == 900)
		return APPLE_1152x900;
	if (xsize == 1280 && ysize == 1024)
		return APPLE_1280x1024;
	if (xsize == 1600 && ysize == 1200)
		return APPLE_1600x1200;
	return APPLE_CUSTOM;
}


/*
 *  Get requested screen size from prefs ("screen win/W/H" or "dga/W/H")
 */

static void get_screen_size(unsigned &width, unsigned &height)
{
	rpi_fb_preferred_size(&width, &height);
	const char *mode_str = PrefsFindString("screen");
	if (mode_str) {
		unsigned w, h;
		if (sscanf(mode_str, "win/%u/%u", &w, &h) == 2 ||
			sscanf(mode_str, "dga/%u/%u", &w, &h) == 2 ||
			sscanf(mode_str, "fb/%u/%u", &w, &h) == 2 ||
			sscanf(mode_str, "%ux%u", &w, &h) == 2) {
			if (w >= 512 && h >= 342 && w <= 1920 && h <= 1200) {
				width = w;
				height = h;
			}
		}
	}
	width &= ~7;	// keep rows nicely aligned
}


/*
 *  Open display (allocate frame buffer)
 */

static bool open_display(void)
{
	const VideoInfo &mode = VModes[cur_mode];

	if (the_buffer == NULL || fb_width != mode.viXsize || fb_height != mode.viYsize) {
		unsigned w, h, pitch;
		uint8 *fb = rpi_fb_init(mode.viXsize, mode.viYsize, 8, &w, &h, &pitch);
		if (fb == NULL)
			return false;
		the_buffer = fb;
		fb_width = w;
		fb_height = h;
		fb_pitch = pitch;
		// Normal cached memory: the Mac does unaligned accesses, which
		// fault on ARM Device memory (cleaned to RAM at 60 Hz)
		rpi_mmu_set_fb_cached((uint32)fb, fb_pitch * fb_height);
	}

	// The Mac owns the screen from now on
	rpi_console_detach();
	memset(the_buffer, 0, fb_pitch * fb_height);

	screen_base = Host2MacAddr(the_buffer);
	display_type = DIS_SCREEN;
	D(bug("Frame buffer at %p (Mac %08x), %dx%d pitch %d\n", the_buffer, screen_base, fb_width, fb_height, fb_pitch));
	return true;
}


/*
 *  Initialization
 */

bool VideoInit(void)
{
	unsigned width, height;
	get_screen_size(width, height);

	// Allocate the frame buffer first to learn the real pitch
	unsigned w, h, pitch;
	uint8 *fb = rpi_fb_init(width, height, 8, &w, &h, &pitch);
	if (fb == NULL) {
		width = 640;
		height = 480;
		fb = rpi_fb_init(width, height, 8, &w, &h, &pitch);
		if (fb == NULL) {
			ErrorAlert(GetString(STR_OPEN_WINDOW_ERR));
			return false;
		}
	}
	the_buffer = fb;
	fb_width = w;
	fb_height = h;
	fb_pitch = pitch;
	rpi_mmu_set_fb_cached((uint32)fb, fb_pitch * fb_height);

	// One 8 bit full screen mode
	VideoInfo *p = VModes;
	p->viType = DIS_SCREEN;
	p->viXsize = fb_width;
	p->viYsize = fb_height;
	p->viRowBytes = fb_pitch;
	p->viAppleMode = APPLE_8_BIT;
	p->viAppleID = find_apple_resolution(fb_width, fb_height);
	p++;
	p->viType = DIS_INVALID;	// End marker
	p->viRowBytes = 0;
	p->viXsize = p->viYsize = 0;
	p->viAppleMode = 0;
	p->viAppleID = 0;
	cur_mode = 0;

	rpi_log("Video: %ux%u, 256 colors, pitch %u\n", fb_width, fb_height, fb_pitch);

	// Default palette: gray ramp (until Mac OS sets its own)
	for (int i = 0; i < 256; i++) {
		mac_pal[i].red = mac_pal[i].green = mac_pal[i].blue = 255 - i;
		mac_pal[i].alpha = 0;
	}

	// Absolute mouse positioning (main_circle.cpp tracks the cursor)
	ADBSetRelMouseMode(false);

	if (!open_display())
		return false;
	video_set_palette();
	video_activated = true;
	return true;
}


/*
 *  Deinitialization
 */

void VideoExit(void)
{
	video_activated = false;
}


/*
 *  Close screen in full-screen mode
 */

void VideoQuitFullScreen(void)
{
}


/*
 *  Execute video VBL routine
 */

void VideoVBL(void)
{
	// Execute video VBL
	if (private_data != NULL && private_data->interruptsEnabled)
		VSLDoInterruptService(private_data->vslServiceID);
}


/*
 *  Change video mode
 */

int16 video_mode_change(VidLocals *csSave, uint32 ParamPtr)
{
	/* return if no mode change */
	if ((csSave->saveData == ReadMacInt32(ParamPtr + csData)) &&
	    (csSave->saveMode == ReadMacInt16(ParamPtr + csMode))) return noErr;

	/* first find video mode in table */
	for (int i=0; VModes[i].viType != DIS_INVALID; i++) {
		if ((ReadMacInt16(ParamPtr + csMode) == VModes[i].viAppleMode) &&
		    (ReadMacInt32(ParamPtr + csData) == VModes[i].viAppleID)) {
			csSave->saveMode = ReadMacInt16(ParamPtr + csMode);
			csSave->saveData = ReadMacInt32(ParamPtr + csData);
			csSave->savePage = ReadMacInt16(ParamPtr + csPage);

			DisableInterrupt();
			cur_mode = i;
			bool ok = open_display();
			if (!ok) {
				ErrorAlert(GetString(STR_FULL_SCREEN_ERR));
				QuitEmulator();
			}
			video_set_palette();

			WriteMacInt32(ParamPtr + csBaseAddr, screen_base);
			csSave->saveBaseAddr=screen_base;
			csSave->saveData=VModes[cur_mode].viAppleID;/* First mode ... */
			csSave->saveMode=VModes[cur_mode].viAppleMode;

			EnableInterrupt();
			return noErr;
		}
	}
	return paramErr;
}


/*
 *  Set color palette
 */

void video_set_palette(void)
{
	uint8 rgb[256 * 3];
	for (int i = 0; i < 256; i++) {
		rgb[i * 3 + 0] = mac_pal[i].red;
		rgb[i * 3 + 1] = mac_pal[i].green;
		rgb[i * 3 + 2] = mac_pal[i].blue;
	}
	rpi_fb_set_palette(0, 256, rgb);
}


/*
 *  Set gamma table (not implemented)
 */

void video_set_gamma(int n_colors)
{
}


/*
 *  Can we set the MacOS cursor image into the window? No: Mac OS draws
 *  the cursor into the frame buffer itself.
 */

bool video_can_change_cursor(void)
{
	return false;
}


/*
 *  Set cursor image for window
 */

void video_set_cursor(void)
{
}


/*
 *  Record dirty area from NQD (nothing to do, we display the frame buffer directly)
 */

void video_set_dirty_area(int x, int y, int w, int h)
{
}
