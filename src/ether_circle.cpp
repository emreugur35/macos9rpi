/*
 *  ether_circle.cpp - Ethernet device driver, bare-metal Circle
 *
 *  macos9rpi - Mac OS 9 on bare-metal Raspberry Pi
 *
 *  Connects SheepShaver's DLPI Ethernet driver (ether.cpp) to the network
 *  device in rpi_net.cpp: on-board Wi-Fi ("ether wlan") or a USB Ethernet
 *  adapter ("ether usb"). Frames are passed 1:1, so Mac OS appears on the
 *  LAN with the device's MAC address and gets its own address via DHCP.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"

#include <string.h>
#include <strings.h>

#include "cpu_emulation.h"
#include "main.h"
#include "macos_util.h"
#include "prefs.h"
#include "ether.h"
#include "ether_defs.h"
#include "rpi_glue.h"

#define DEBUG 0
#include "debug.h"

static bool net_open = false;		// Network device started
static bool addr_logged = false;

// Used until a USB adapter shows up (and if the network is off)
static const uint8 fallback_addr[6] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};

#define MAX_MULTICAST 32
static uint8 multicast_addrs[MAX_MULTICAST][6];
static int num_multicast = 0;

#define MAX_RX_PER_IRQ 32

// "etherdebug true": log the first frames in each direction
static bool ether_debug = false;
static int debug_rx = 0, debug_tx = 0;
#define DEBUG_FRAMES 40

static void log_frame(const char *dir, const uint8 *p, unsigned len)
{
	rpi_log("Ethernet %s %u: %02x:%02x:%02x:%02x:%02x:%02x > %02x:%02x:%02x:%02x:%02x:%02x type %02x%02x\n",
			dir, len, p[6], p[7], p[8], p[9], p[10], p[11], p[0], p[1], p[2], p[3], p[4], p[5], p[12], p[13]);
}


/*
 *  Initialization
 */

void EtherInit(void)
{
	net_open = false;

	if (PrefsFindBool("nonet"))
		return;

	const char *mode = PrefsFindString("ether");
	if (mode == NULL || *mode == 0 || strcasecmp(mode, "none") == 0 || strcasecmp(mode, "off") == 0)
		return;

	ether_debug = PrefsFindBool("etherdebug");
	rpi_net_set_debug(ether_debug);
	net_open = rpi_net_init(mode) == 0;
}

void EtherExit(void)
{
	if (net_open) {
		rpi_log("Ethernet: %d packets received, %d sent (%d IRQs)\n",
				num_rx_packets, num_tx_packets, num_ether_irq);
		rpi_net_log_stats();
	}
	net_open = false;
}


/*
 *  Poll the network device (main context: 60 Hz and when Mac OS is idle)
 */

void EtherCirclePoll(void)
{
	if (!net_open)
		return;

	if (rpi_net_poll() != 0 && !(InterruptFlags & INTFLAG_ETHER)) {
		SetInterruptFlag(INTFLAG_ETHER);
		TriggerInterrupt();
	}
}


/*
 *  Get ethernet hardware address
 */

void AO_get_ethernet_address(uint32 arg)
{
	uint8 *addr = Mac2HostAddr(arg);
	uint8 mac[6];
	if (net_open && rpi_net_get_mac(mac)) {
		memcpy(addr, mac, 6);
		if (!addr_logged) {
			addr_logged = true;
			rpi_log("Ethernet: Mac OS uses MAC address %02x:%02x:%02x:%02x:%02x:%02x\n",
					mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		}
	} else
		memcpy(addr, fallback_addr, 6);
	D(bug("AO_get_ethernet_address: %02x%02x%02x%02x%02x%02x\n", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]));
}


/*
 *  Multicast addresses
 */

static void update_multicast(void)
{
	rpi_net_set_multicast(&multicast_addrs[0][0], num_multicast);
}

void AO_enable_multicast(uint32 addr)
{
	if (!net_open)
		return;
	const uint8 *a = Mac2HostAddr(addr);
	for (int i = 0; i < num_multicast; i++)
		if (memcmp(multicast_addrs[i], a, 6) == 0)
			return;
	if (num_multicast >= MAX_MULTICAST)
		return;
	memcpy(multicast_addrs[num_multicast++], a, 6);
	update_multicast();
}

void AO_disable_multicast(uint32 addr)
{
	if (!net_open)
		return;
	const uint8 *a = Mac2HostAddr(addr);
	for (int i = 0; i < num_multicast; i++) {
		if (memcmp(multicast_addrs[i], a, 6) == 0) {
			memmove(multicast_addrs[i], multicast_addrs[i + 1], (num_multicast - i - 1) * 6);
			num_multicast--;
			update_multicast();
			return;
		}
	}
}


/*
 *  Transmit one packet (STREAMS message block chain)
 */

void AO_transmit_packet(uint32 mp)
{
	if (!net_open)
		return;

	uint8 packet[1514];
	uint32 len = 0;
	while (mp) {
		uint32 rptr = ReadMacInt32(mp + 12);
		uint32 size = ReadMacInt32(mp + 16) - rptr;
		if (len + size > sizeof(packet)) {
			num_tx_buffer_full++;
			return;
		}
		Mac2Host_memcpy(packet + len, rptr, size);
		len += size;
		mp = ReadMacInt32(mp + 8);
	}

	if (ether_debug && debug_tx < DEBUG_FRAMES && len >= 14) {
		debug_tx++;
		log_frame("tx", packet, len);
	}

	if (rpi_net_send(packet, len) == 0)
		num_tx_packets++;
	else
		num_tx_buffer_full++;
}


/*
 *  Ethernet interrupt: hand received packets to Open Transport
 */

void EtherIRQ(void)
{
	D(bug("EtherIRQ\n"));
	num_ether_irq++;

	OTEnterInterrupt();
	EthernetPacket ether_packet;
	uint32 packet = ether_packet.addr();
	int n = 0;
	unsigned length;
	while (n < MAX_RX_PER_IRQ && (length = rpi_net_receive(Mac2HostAddr(packet))) != 0) {
		if (ether_debug && debug_rx < DEBUG_FRAMES) {
			debug_rx++;
			log_frame("rx", Mac2HostAddr(packet), length);
		}
		ether_dispatch_packet(packet, length);
		n++;
	}
	OTLeaveInterrupt();

	// More waiting? Come back after the other interrupt sources had a turn
	if (n == MAX_RX_PER_IRQ) {
		SetInterruptFlag(INTFLAG_ETHER);
		TriggerInterrupt();
	}
}
