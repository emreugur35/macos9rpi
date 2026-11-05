/*
 *  sys_circle.cpp - System dependent routines (disk/CD access) for the
 *                   bare-metal Raspberry Pi (Circle) port of SheepShaver
 *
 *  Supported "disk" / "cdrom" prefs values:
 *
 *    <file name>        image file on the SD card's FAT partition (max 4 GB)
 *    sd:p<N>            raw partition N (1..4) of the SD card's MBR,
 *                       e.g. "sd:p2" for a raw HFS/HFS+ volume or disk image
 *                       written byte-for-byte into partition 2 (no size limit)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"

#include <fatfs/ff.h>
#include <strings.h>

#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "user_strings.h"
#include "sys.h"
#include "rpi_glue.h"

#define DEBUG 0
#include "debug.h"


// File handles are pointers to these structures
struct mac_file_handle {
	char *name;			// Name as given in prefs
	bool read_only;		// Copy of Sys_open() flag
	bool is_cdrom;		// Opened as CD-ROM
	bool is_raw;		// Raw SD card partition (otherwise FAT file)
	FIL fil;			// FatFs file object (file)
	DWORD *clmt;		// FatFs fast seek cluster link map table
	uint64 raw_start;	// Start of raw partition (bytes)
	loff_t start_byte;	// Size of image header (e.g. DiskCopy 4.2)
	loff_t file_size;	// Usable size
	bool cd_locked;
	bool dirty;			// FAT file written since last f_sync()
};

// Bounce buffer for raw SD card access (aligned, multiple of 512)
static const uint32 BOUNCE_SIZE = 256 * 1024;
static uint8 *bounce_buffer = NULL;


/*
 *  Read cache
 *
 *  SD cards are slow at small random reads (each command costs ~0.5-2 ms),
 *  and Mac OS reads its catalog, resources and extensions in many small
 *  pieces. We therefore read 32 KB chunks (with neighbouring missing chunks
 *  of the same request in one multi-block command) and keep them in RAM.
 *  Writes go straight through to the card and update cached chunks.
 */

static const uint32 CHUNK_SHIFT = 15;
static const uint32 CHUNK_SIZE = 1 << CHUNK_SHIFT;		// 32 KB
static const uint32 MAX_FETCH_CHUNKS = BOUNCE_SIZE / CHUNK_SIZE;

struct cache_entry {
	mac_file_handle *fh;
	uint32 chunk;
	int next;			// hash chain
	uint32 len;			// valid bytes (last chunk of a disk may be short)
	bool used;
	bool ref;			// referenced since the clock hand passed
	uint8 *data;
};

static cache_entry *cache = NULL;
static int *cache_hash = NULL;
static int cache_n = 0, cache_hash_n = 0, cache_hand = 0;
static uint32 cache_hits = 0, cache_misses = 0;
static uint32 sd_read_cmds = 0, sd_read_kb = 0;		// statistics

static inline int cache_bucket(mac_file_handle *fh, uint32 chunk)
{
	return (int)((((uintptr)fh >> 4) * 2654435761u ^ chunk * 40503u) % (uint32)cache_hash_n);
}

static int cache_lookup(mac_file_handle *fh, uint32 chunk)
{
	for (int i = cache_hash[cache_bucket(fh, chunk)]; i >= 0; i = cache[i].next)
		if (cache[i].fh == fh && cache[i].chunk == chunk)
			return i;
	return -1;
}

static void cache_unlink(int i)
{
	int *pp = &cache_hash[cache_bucket(cache[i].fh, cache[i].chunk)];
	while (*pp >= 0) {
		if (*pp == i) {
			*pp = cache[i].next;
			break;
		}
		pp = &cache[*pp].next;
	}
	cache[i].used = false;
}

static int cache_alloc(void)
{
	for (;;) {
		int i = cache_hand;
		cache_hand = (cache_hand + 1) % cache_n;
		if (!cache[i].used)
			return i;
		if (cache[i].ref) {
			cache[i].ref = false;
			continue;
		}
		cache_unlink(i);
		return i;
	}
}

static void cache_insert(int i, mac_file_handle *fh, uint32 chunk, uint32 len)
{
	int b = cache_bucket(fh, chunk);
	cache[i].fh = fh;
	cache[i].chunk = chunk;
	cache[i].len = len;
	cache[i].used = true;
	cache[i].ref = true;
	cache[i].next = cache_hash[b];
	cache_hash[b] = i;
}

static void cache_drop_file(mac_file_handle *fh)
{
	for (int i = 0; i < cache_n; i++)
		if (cache[i].used && cache[i].fh == fh)
			cache_unlink(i);
}

static void cache_init(void)
{
	int32 mb = PrefsFindInt32("diskcache");
	if (mb <= 0)
		return;
	if (mb > 128)
		mb = 128;
	int n = (mb << 20) / CHUNK_SIZE;
	uint8 *mem = (uint8 *)malloc((size_t)n * CHUNK_SIZE + 64);
	if (mem == NULL) {
		rpi_log("Not enough memory for a %d MB disk cache\n", (int)mb);
		return;
	}
	mem = (uint8 *)(((uintptr)mem + 63) & ~63);
	cache = new cache_entry[n];
	cache_hash_n = n * 2 + 1;
	cache_hash = new int[cache_hash_n];
	for (int i = 0; i < cache_hash_n; i++)
		cache_hash[i] = -1;
	for (int i = 0; i < n; i++) {
		cache[i].used = false;
		cache[i].ref = false;
		cache[i].data = mem + (size_t)i * CHUNK_SIZE;
	}
	cache_n = n;
	rpi_log("Disk read cache: %d MB\n", (int)mb);
}


/*
 *  Initialization
 */

void SysInit(void)
{
	if (bounce_buffer == NULL) {
		uint8 *p = (uint8 *)malloc(BOUNCE_SIZE + 64);
		bounce_buffer = (uint8 *)(((uintptr)p + 63) & ~63);
	}
	if (cache == NULL)
		cache_init();
}


/*
 *  Write pending FAT metadata (called periodically and on close)
 */

static mac_file_handle *open_handles[16];

void SysPeriodicFlush(void)
{
	for (int i = 0; i < 16; i++) {
		mac_file_handle *fh = open_handles[i];
		if (fh && fh->dirty && !fh->is_raw) {
			f_sync(&fh->fil);
			fh->dirty = false;
		}
	}
}

void SysCacheStats(uint32 *hits, uint32 *misses)
{
	*hits = cache_hits;
	*misses = cache_misses;
}

void SysReadStats(uint32 *cmds, uint32 *kb)
{
	*cmds = sd_read_cmds;
	*kb = sd_read_kb;
}


/*
 *  Deinitialization
 */

void SysExit(void)
{
}


/*
 *  These functions add the default devices to the prefs (none here, the
 *  images are added by prefs_circle.cpp)
 */

void SysAddFloppyPrefs(void)
{
}

void SysAddDiskPrefs(void)
{
}

void SysAddCDROMPrefs(void)
{
}

void SysAddSerialPrefs(void)
{
}


/*
 *  Disk image layout detection (from BasiliskII macos_util.cpp)
 */

static void file_disk_layout(loff_t size, loff_t &start_byte, loff_t &real_size)
{
	if (size == 419284 || size == 838484) {
		// 400K/800K DiskCopy image, 84 byte header
		start_byte = 84;
		real_size = (size - 84) & ~0x1ff;
	} else {
		// 0..511 byte header
		start_byte = size & 0x1ff;
		real_size = size - start_byte;
	}
}


/*
 *  Raw SD card partitions
 */

static bool find_sd_partition(int n, uint64 &start, uint64 &size)
{
	if (n < 1 || n > 4)
		return false;
	uint8 *mbr = bounce_buffer;
	if (rpi_sd_read(0, mbr, 512) != 512)
		return false;
	if (mbr[510] != 0x55 || mbr[511] != 0xaa)
		return false;
	const uint8 *e = mbr + 446 + (n - 1) * 16;
	uint32 lba = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32)e[11] << 24);
	uint32 count = e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32)e[15] << 24);
	if (e[4] == 0 || count == 0)
		return false;
	start = (uint64)lba * 512;
	size = (uint64)count * 512;
	return true;
}

static size_t raw_read(uint64 pos, uint8 *buf, size_t length)
{
	size_t done = 0;
	while (done < length) {
		uint64 sector_pos = pos & ~(uint64)511;
		uint32 skip = (uint32)(pos - sector_pos);
		uint32 chunk = BOUNCE_SIZE - skip;
		if (chunk > length - done)
			chunk = length - done;
		uint32 io_len = (skip + chunk + 511) & ~511;
		if (rpi_sd_read(sector_pos, bounce_buffer, io_len) != (int)io_len)
			break;
		memcpy(buf + done, bounce_buffer + skip, chunk);
		done += chunk;
		pos += chunk;
	}
	return done;
}

static size_t raw_write(uint64 pos, const uint8 *buf, size_t length)
{
	size_t done = 0;
	while (done < length) {
		uint64 sector_pos = pos & ~(uint64)511;
		uint32 skip = (uint32)(pos - sector_pos);
		uint32 chunk = BOUNCE_SIZE - skip;
		if (chunk > length - done)
			chunk = length - done;
		uint32 io_len = (skip + chunk + 511) & ~511;
		// Partial first/last sector: read-modify-write
		if (skip != 0 || (chunk & 511) != 0) {
			if (rpi_sd_read(sector_pos, bounce_buffer, io_len) != (int)io_len)
				break;
		}
		memcpy(bounce_buffer + skip, buf + done, chunk);
		if (rpi_sd_write(sector_pos, bounce_buffer, io_len) != (int)io_len)
			break;
		done += chunk;
		pos += chunk;
	}
	return done;
}


/*
 *  Uncached read from the underlying device (offset relative to disk start)
 */

static size_t backend_read(mac_file_handle *fh, loff_t offset, uint8 *buf, size_t length)
{
	sd_read_cmds++;
	sd_read_kb += length >> 10;
	if (fh->is_raw) {
		uint64 pos = fh->raw_start + offset + fh->start_byte;
		if ((pos & 511) == 0 && (length & 511) == 0 && ((uintptr)buf & 3) == 0) {
			// Directly into the (aligned) destination, one multi-block command
			if (rpi_sd_read(pos, buf, length) == (int)length)
				return length;
			return 0;
		}
		return raw_read(pos, buf, length);
	}

	if (f_lseek(&fh->fil, (FSIZE_t)(offset + fh->start_byte)) != FR_OK)
		return 0;
	UINT actual = 0;
	if (f_read(&fh->fil, buf, length, &actual) != FR_OK)
		return 0;
	return actual;
}


/*
 *  Open file/device, create new file handle (returns NULL on error)
 */

void *Sys_open(const char *name, bool read_only, bool is_cdrom)
{
	if (name == NULL || *name == 0)
		return NULL;

	mac_file_handle *fh = new mac_file_handle;
	memset(fh, 0, sizeof(*fh));
	fh->name = strdup(name);
	fh->is_cdrom = is_cdrom;
	if (is_cdrom)
		read_only = true;

	if (strncasecmp(name, "sd:p", 4) == 0) {
		// Raw partition of the SD card
		uint64 start, size;
		if (!find_sd_partition(atoi(name + 4), start, size)) {
			rpi_log("Cannot find SD card partition \"%s\"\n", name);
			free(fh->name);
			delete fh;
			return NULL;
		}
		fh->is_raw = true;
		fh->raw_start = start;
		fh->start_byte = 0;
		fh->file_size = size;
		fh->read_only = read_only;
		rpi_log("Disk \"%s\": raw SD partition, %u MB%s\n", name,
				(unsigned)(size >> 20), read_only ? " (read-only)" : "");
		for (int i = 0; i < 16; i++)
			if (open_handles[i] == NULL) { open_handles[i] = fh; break; }
		return fh;
	}

	// Image file on the FAT partition
	BYTE mode = FA_READ | (read_only ? 0 : FA_WRITE);
	FRESULT res = f_open(&fh->fil, name, mode);
	if (res != FR_OK && !read_only) {
		read_only = true;
		res = f_open(&fh->fil, name, FA_READ);
	}
	if (res != FR_OK) {
		rpi_log("Cannot open disk image \"%s\" (FatFs error %d)\n", name, (int)res);
		free(fh->name);
		delete fh;
		return NULL;
	}
	fh->read_only = read_only;

	// Fast seek: build the cluster link map table (the file size cannot
	// change any more after that, which is fine for disk images)
	DWORD tmp[4];
	fh->fil.cltbl = tmp;
	tmp[0] = 4;
	res = f_lseek(&fh->fil, CREATE_LINKMAP);
	if (res == FR_NOT_ENOUGH_CORE) {
		DWORD needed = tmp[0];
		fh->clmt = new DWORD[needed];
		fh->clmt[0] = needed;
		fh->fil.cltbl = fh->clmt;
		res = f_lseek(&fh->fil, CREATE_LINKMAP);
	} else if (res == FR_OK) {
		fh->clmt = new DWORD[4];
		memcpy(fh->clmt, tmp, sizeof(tmp));
		fh->fil.cltbl = fh->clmt;
	}
	if (res != FR_OK) {
		// Fall back to normal (slow) seeking
		fh->fil.cltbl = NULL;
		delete[] fh->clmt;
		fh->clmt = NULL;
	}

	loff_t size = f_size(&fh->fil);
	file_disk_layout(size, fh->start_byte, fh->file_size);
	rpi_log("Disk \"%s\": %u MB%s%s\n", name, (unsigned)(fh->file_size >> 20),
			read_only ? " (read-only)" : "", fh->clmt ? "" : " (slow seek)");
	for (int i = 0; i < 16; i++)
		if (open_handles[i] == NULL) { open_handles[i] = fh; break; }
	return fh;
}


/*
 *  Close file/device, delete file handle
 */

void Sys_close(void *arg)
{
	mac_file_handle *fh = (mac_file_handle *)arg;
	if (!fh)
		return;
	if (cache)
		cache_drop_file(fh);
	for (int i = 0; i < 16; i++)
		if (open_handles[i] == fh)
			open_handles[i] = NULL;
	if (!fh->is_raw) {
		f_close(&fh->fil);
		delete[] fh->clmt;
	}
	free(fh->name);
	delete fh;
}


/*
 *  Read "length" bytes from file/device, starting at "offset", to "buffer",
 *  returns number of bytes read (or 0)
 */

size_t Sys_read(void *arg, void *buffer, loff_t offset, size_t length)
{
	mac_file_handle *fh = (mac_file_handle *)arg;
	if (!fh || offset < 0)
		return 0;
	if (offset >= fh->file_size)
		return 0;
	if ((loff_t)length > fh->file_size - offset)
		length = fh->file_size - offset;

	if (cache == NULL)
		return backend_read(fh, offset, (uint8 *)buffer, length);

	uint8 *dst = (uint8 *)buffer;
	size_t done = 0;
	while (done < length) {
		loff_t pos = offset + done;
		uint32 chunk = (uint32)(pos >> CHUNK_SHIFT);
		uint32 within = (uint32)(pos & (CHUNK_SIZE - 1));
		uint32 n = CHUNK_SIZE - within;
		if (n > length - done)
			n = length - done;

		int i = cache_lookup(fh, chunk);
		if (i >= 0) {
			cache_hits++;
		} else {
			cache_misses++;
			// Fetch this chunk and following missing chunks of the same
			// request with one multi-block read
			uint32 last_needed = (uint32)((offset + length - 1) >> CHUNK_SHIFT);
			uint32 count = 1;
			while (count < MAX_FETCH_CHUNKS && chunk + count <= last_needed &&
				   cache_lookup(fh, chunk + count) < 0)
				count++;
			loff_t fetch_pos = (loff_t)chunk << CHUNK_SHIFT;
			loff_t avail = fh->file_size - fetch_pos;
			uint32 fetch_len = count * CHUNK_SIZE;
			if ((loff_t)fetch_len > avail)
				fetch_len = (uint32)avail;
			if (backend_read(fh, fetch_pos, bounce_buffer, fetch_len) != fetch_len)
				return done;
			for (uint32 c = 0; c < count; c++) {
				uint32 clen = fetch_len - c * CHUNK_SIZE;
				if (clen > CHUNK_SIZE)
					clen = CHUNK_SIZE;
				int e = cache_alloc();
				memcpy(cache[e].data, bounce_buffer + c * CHUNK_SIZE, clen);
				cache_insert(e, fh, chunk + c, clen);
			}
			i = cache_lookup(fh, chunk);
		}
		cache[i].ref = true;
		if (within >= cache[i].len)
			break;
		if (n > cache[i].len - within)
			n = cache[i].len - within;
		memcpy(dst + done, cache[i].data + within, n);
		done += n;
	}
	return done;
}


/*
 *  Write "length" bytes from "buffer" to file/device, starting at "offset",
 *  returns number of bytes written (or 0)
 */

size_t Sys_write(void *arg, void *buffer, loff_t offset, size_t length)
{
	mac_file_handle *fh = (mac_file_handle *)arg;
	if (!fh || fh->read_only || offset < 0)
		return 0;
	if (offset >= fh->file_size)
		return 0;
	if ((loff_t)length > fh->file_size - offset)
		length = fh->file_size - offset;

	size_t actual;
	if (fh->is_raw) {
		actual = raw_write(fh->raw_start + offset + fh->start_byte, (const uint8 *)buffer, length);
	} else {
		if (f_lseek(&fh->fil, (FSIZE_t)(offset + fh->start_byte)) != FR_OK)
			return 0;
		UINT n = 0;
		if (f_write(&fh->fil, buffer, length, &n) != FR_OK)
			return 0;
		actual = n;
		fh->dirty = true;
	}

	// Keep cached chunks up to date
	if (cache && actual) {
		const uint8 *src = (const uint8 *)buffer;
		size_t done = 0;
		while (done < actual) {
			loff_t pos = offset + done;
			uint32 chunk = (uint32)(pos >> CHUNK_SHIFT);
			uint32 within = (uint32)(pos & (CHUNK_SIZE - 1));
			uint32 n = CHUNK_SIZE - within;
			if (n > actual - done)
				n = actual - done;
			int i = cache_lookup(fh, chunk);
			if (i >= 0 && within < cache[i].len) {
				uint32 m = n;
				if (m > cache[i].len - within)
					m = cache[i].len - within;
				memcpy(cache[i].data + within, src + done, m);
			}
			done += n;
		}
	}
	return actual;
}


/*
 *  Return size of file/device (minus header)
 */

loff_t SysGetFileSize(void *arg)
{
	mac_file_handle *fh = (mac_file_handle *)arg;
	if (!fh)
		return 0;
	return fh->file_size;
}


/*
 *  Eject volume (if applicable)
 */

void SysEject(void *arg)
{
}


/*
 *  Format volume (if applicable)
 */

bool SysFormat(void *arg)
{
	return false;
}


/*
 *  Check if file/device is read-only (this includes the read-only flag on Sys_open())
 */

bool SysIsReadOnly(void *arg)
{
	mac_file_handle *fh = (mac_file_handle *)arg;
	if (!fh)
		return true;
	return fh->read_only;
}


/*
 *  Check if the given file handle refers to a fixed or a removable disk
 */

bool SysIsFixedDisk(void *arg)
{
	mac_file_handle *fh = (mac_file_handle *)arg;
	if (!fh)
		return true;
	return !fh->is_cdrom;
}


/*
 *  Check if a disk is inserted in the drive (always true for files)
 */

bool SysIsDiskInserted(void *arg)
{
	mac_file_handle *fh = (mac_file_handle *)arg;
	return fh != NULL;
}


/*
 *  Prevent/allow medium removal (if applicable)
 */

void SysPreventRemoval(void *arg)
{
	mac_file_handle *fh = (mac_file_handle *)arg;
	if (fh)
		fh->cd_locked = true;
}

void SysAllowRemoval(void *arg)
{
	mac_file_handle *fh = (mac_file_handle *)arg;
	if (fh)
		fh->cd_locked = false;
}


/*
 *  Read CD-ROM TOC (binary MSF format, 804 bytes max.)
 *  For image files a TOC with one data track is synthesized.
 */

static void lba_to_msf(uint32 lba, uint8 *m, uint8 *s, uint8 *f)
{
	lba += 150;		// 2 second pregap
	*m = lba / (60 * 75);
	*s = (lba / 75) % 60;
	*f = lba % 75;
}

bool SysCDReadTOC(void *arg, uint8 *toc)
{
	mac_file_handle *fh = (mac_file_handle *)arg;
	if (!fh)
		return false;

	uint8 *p = toc + 2;
	*p++ = 1;		// first track
	*p++ = 1;		// last track

	// Track 1: data track at LBA 0
	*p++ = 0;
	*p++ = 0x14;	// ADR 1, data track
	*p++ = 1;
	*p++ = 0;
	*p++ = 0;
	lba_to_msf(0, p, p + 1, p + 2);
	p += 3;

	// Lead-out
	uint32 blocks = (uint32)(fh->file_size / 2048);
	*p++ = 0;
	*p++ = 0x14;
	*p++ = 0xaa;
	*p++ = 0;
	*p++ = 0;
	lba_to_msf(blocks, p, p + 1, p + 2);
	p += 3;

	int len = p - toc;
	toc[0] = len >> 8;
	toc[1] = len;
	return true;
}


/*
 *  Audio CD functions (not supported for images)
 */

bool SysCDGetPosition(void *arg, uint8 *pos)
{
	return false;
}

bool SysCDPlay(void *arg, uint8 start_m, uint8 start_s, uint8 start_f, uint8 end_m, uint8 end_s, uint8 end_f)
{
	return false;
}

bool SysCDPause(void *arg)
{
	return false;
}

bool SysCDResume(void *arg)
{
	return false;
}

bool SysCDStop(void *arg, uint8 lead_out_m, uint8 lead_out_s, uint8 lead_out_f)
{
	return false;
}

bool SysCDScan(void *arg, uint8 start_m, uint8 start_s, uint8 start_f, bool reverse)
{
	return false;
}

void SysCDSetVolume(void *arg, uint8 left, uint8 right)
{
}

void SysCDGetVolume(void *arg, uint8 &left, uint8 &right)
{
	left = right = 0;
}
