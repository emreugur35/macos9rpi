/*
 *  prefs_circle.cpp - Preferences handling, bare-metal Circle specific stuff
 *
 *  Preferences are read from "macos9rpi.txt" in the root of the SD card's
 *  FAT partition (SheepShaver prefs format: "keyword value" per line).
 *  If no disk/cdrom is configured, disk and CD images found in the root
 *  directory are used automatically.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"

#include <string>
#include <ctype.h>
#include <dirent.h>
#include <strings.h>

#include "prefs.h"
#include "rpi_glue.h"

// Platform-specific preferences items
prefs_desc platform_prefs_items[] = {
	{"idlewait", TYPE_BOOLEAN, false,      "sleep when idle"},
	{"diskcache", TYPE_INT32, false,       "disk read cache size in MB (0 = off)"},
	{"mousespeed", TYPE_INT32, false,      "mouse speed in percent (default 100)"},
	{"profile", TYPE_BOOLEAN, false,       "write PowerPC pc samples to macos9rpi.prof at shutdown"},
	{"jittest", TYPE_INT32, false,         "JIT self test iterations per instruction form (default 20)"},
	{"jitprofile", TYPE_BOOLEAN, false,    "count executed PowerPC instructions (slow, log only)"},
	{"jitsample", TYPE_BOOLEAN, false,     "log where the time goes (60 Hz samples, log only)"},
	{"ether", TYPE_STRING, false,          "network: wlan (Wi-Fi) or usb (USB Ethernet adapter)"},
	{"etherconfig", TYPE_STRING, false,    "unused"},
	{"etherdebug", TYPE_BOOLEAN, false,    "log the first network frames"},
	{"keycodes", TYPE_BOOLEAN, false,      "unused"},
	{"keycodefile", TYPE_STRING, false,    "unused"},
	{"mousewheelmode", TYPE_INT32, false,  "unused"},
	{"mousewheellines", TYPE_INT32, false, "unused"},
	{"dsp", TYPE_STRING, false,            "unused"},
	{"mixer", TYPE_STRING, false,          "unused"},
	{NULL, TYPE_END, false, NULL} // End of list
};

static const char PREFS_FILE_NAME[] = "macos9rpi.txt";

std::string UserPrefsPath;


/*
 *  Does the file name end with one of the given extensions?
 */

static bool has_extension(const char *name, const char *const *exts)
{
	const char *dot = strrchr(name, '.');
	if (dot == NULL)
		return false;
	for (int i = 0; exts[i]; i++)
		if (strcasecmp(dot + 1, exts[i]) == 0)
			return true;
	return false;
}


/*
 *  Look for disk and CD-ROM images in the root directory
 */

static void auto_detect_images(void)
{
	static const char *const disk_exts[] = {"img", "dsk", "hfv", "hda", "hfs", "image", "vhd", NULL};
	static const char *const cd_exts[] = {"iso", "toast", "cdr", NULL};

	bool have_disk = PrefsFindString("disk") != NULL;
	bool have_cdrom = PrefsFindString("cdrom") != NULL;
	if (have_disk && have_cdrom)
		return;

	// Raw Mac disks in SD card partitions 2..4 (MBR type 0xAF)
	if (!have_disk) {
		static uint8 mbr_raw[512 + 64];
		uint8 *mbr = (uint8 *)(((uintptr)mbr_raw + 63) & ~63);
		if (rpi_sd_read(0, mbr, 512) == 512 && mbr[510] == 0x55 && mbr[511] == 0xaa) {
			for (int n = 2; n <= 4; n++) {
				if (mbr[446 + (n - 1) * 16 + 4] == 0xaf) {
					char name[8];
					snprintf(name, sizeof(name), "sd:p%d", n);
					rpi_log("Found Mac disk partition: %s\n", name);
					PrefsAddString("disk", name);
				}
			}
		}
	}

	DIR *dir = opendir("/");
	if (dir == NULL)
		return;
	struct dirent *de;
	while ((de = readdir(dir)) != NULL) {
		const char *name = de->d_name;
		if (name[0] == '.' || name[0] == '_')
			continue;
		if (!have_disk && has_extension(name, disk_exts)) {
			rpi_log("Found disk image: %s\n", name);
			PrefsAddString("disk", name);
		} else if (!have_cdrom && has_extension(name, cd_exts)) {
			rpi_log("Found CD-ROM image: %s\n", name);
			PrefsAddString("cdrom", name);
		}
	}
	closedir(dir);
}


/*
 *  Load preferences from settings file
 */

void LoadPrefs(const char *vmdir)
{
	FILE *f = fopen(PREFS_FILE_NAME, "rb");
	if (f != NULL) {
		// Read file and normalize line endings (the file may have been
		// edited on Windows)
		std::string text;
		char buf[512];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			text.append(buf, n);
		fclose(f);

		std::string clean;
		for (size_t i = 0; i < text.size(); i++) {
			char c = text[i];
			if (c == '\r')
				c = '\n';
			clean += c;
		}
		// Strip trailing white space of every line
		std::string out;
		size_t pos = 0;
		while (pos < clean.size()) {
			size_t eol = clean.find('\n', pos);
			if (eol == std::string::npos)
				eol = clean.size();
			std::string line = clean.substr(pos, eol - pos);
			while (!line.empty() && isspace((unsigned char)line[line.size() - 1]))
				line.erase(line.size() - 1);
			size_t start = 0;
			while (start < line.size() && isspace((unsigned char)line[start]))
				start++;
			line = line.substr(start);
			if (!line.empty()) {
				out += line;
				out += '\n';
			}
			pos = eol + 1;
		}

		FILE *mf = fmemopen((void *)out.data(), out.size(), "r");
		if (mf != NULL) {
			LoadPrefsFromStream(mf);
			fclose(mf);
		}
		rpi_log("Using prefs file %s\n", PREFS_FILE_NAME);
	} else {
		rpi_log("No %s found, using defaults\n", PREFS_FILE_NAME);
	}

	auto_detect_images();
}


/*
 *  Save preferences to settings file
 */

void SavePrefs(void)
{
	FILE *f = fopen(PREFS_FILE_NAME, "w");
	if (f != NULL) {
		SavePrefsToStream(f);
		fclose(f);
	}
}


/*
 *  Add defaults of platform-specific prefs items
 *  You may also override the defaults set in PrefsInit()
 */

void AddPlatformPrefsDefaults(void)
{
	PrefsReplaceInt32("ramsize", 128 * 1024 * 1024);
	PrefsReplaceBool("swap_opt_cmd", false);	// Alt = Command, Windows key = Option
	PrefsReplaceBool("nosound", true);
	PrefsReplaceBool("nonet", false);	// network is off unless "ether wlan/usb"
	PrefsReplaceBool("ignoresegv", true);
	PrefsReplaceBool("ignoreillegal", true);
	PrefsReplaceBool("jit", true);		// ARMv6 translator (ppc_armjit.cpp)
	PrefsReplaceBool("jit68k", false);
	PrefsReplaceBool("gfxaccel", true);
	PrefsReplaceString("rom", "Mac OS ROM");
	PrefsAddBool("idlewait", true);
	PrefsAddInt32("diskcache", 32);
	PrefsAddBool("profile", false);
	PrefsAddInt32("mousespeed", 100);
	PrefsAddBool("etherdebug", false);
}


/*
 *  System specific initialization / deinitialization
 */

void prefs_init(void)
{
}

void prefs_exit(void)
{
}
