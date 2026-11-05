//
// rpi_net.cpp - Network device behind the Mac's Ethernet (Circle side)
//
// macos9rpi - Mac OS 9 on bare-metal Raspberry Pi
//
// The Mac's Ethernet frames are passed 1:1 to a network device:
//
//   "wlan"  the on-board Wi-Fi (CYW43438) with Circle's WLAN driver and
//           wpa_supplicant (WPA/WPA2-Personal or open networks)
//   "usb"   a USB Ethernet adapter (SMSC951x, LAN7800, CDC Ethernet;
//           also QEMU's usb-net, which is how the Mac side is tested)
//
// A Wi-Fi client can only send frames with its own MAC address, so the Mac
// simply uses the device's MAC address, and Circle's own TCP/IP stack is not
// started at all. Mac OS gets its own DHCP lease from the router and shows
// up on the LAN like any other computer, i.e. a bridge with one client.
//
// EAPOL frames (the WPA key handshake) are diverted to wpa_supplicant via
// the l2_packet interface at the end of this file. It replaces hostap's
// l2_packet_circle.cpp, which needs Circle's TCP/IP stack.
//
// The WLAN driver and wpa_supplicant are Circle tasks. They only get the
// CPU when the emulator calls rpi_net_poll(), which yields to them (60 Hz
// and whenever Mac OS is idle).
//

#include "rpi_glue.h"

#include <circle/sched/scheduler.h>
#include <circle/netdevice.h>
#include <circle/macaddress.h>
#include <circle/net/netqueue.h>
#include <circle/types.h>
#include <wlan/bcm4343.h>
#include <wlan/hostap/wpa_supplicant/wpasupplicant.h>
#include <fatfs/ff.h>

#include <string.h>
#include <strings.h>
#include <stdlib.h>

#define ETH_HDR_LEN		14
#define ETH_MIN_FRAME		60
#define ETH_MAX_FRAME		1514
#define ETH_P_EAPOL		0x888E

#define WLAN_FIRMWARE_PATH	"SD:/firmware/"
#define WLAN_FIRMWARE_CHECK	"SD:/firmware/brcmfmac43430-sdio.bin"
#define WPA_CONFIG_FILE		"SD:/wpa_supplicant.conf"

#define MAX_MULTICAST		32

enum TNetMode { NetModeNone, NetModeWLAN, NetModeUSB };

static TNetMode		s_Mode = NetModeNone;
static CScheduler      *s_pScheduler = 0;
static CBcm4343Device  *s_pWLAN = 0;
static CWPASupplicant  *s_pWPA = 0;
static CNetDevice      *s_pDevice = 0;
static CNetQueue       *s_pEAPOLQueue = 0;	// EAPOL frames for wpa_supplicant

// Received frames for the Mac (only touched by the emulator thread)
#define RX_RING_SIZE	64
static u8		s_RxRing[RX_RING_SIZE][FRAME_BUFFER_SIZE];
static unsigned		s_RxLength[RX_RING_SIZE];
static unsigned		s_nRxHead = 0;
static unsigned		s_nRxTail = 0;
static unsigned		s_nRxDropped = 0;

#define MAX_RX_PER_POLL		128
#define MIN_POLL_INTERVAL	250		// microseconds
static u64		s_nLastPoll = 0;

static inline unsigned RxPending (void)
{
	return (s_nRxHead + RX_RING_SIZE - s_nRxTail) % RX_RING_SIZE;
}

static bool		s_bLinkUp = false;
static u64		s_nLastLinkCheck = 0;
static u64		s_nLastPHYUpdate = 0;
static unsigned		s_nRxFrames = 0, s_nTxFrames = 0, s_nTxDropped = 0;
static bool		s_bDebug = false;
static unsigned		s_nPolls = 0;
static u64		s_nLastDebug = 0;

void rpi_net_set_debug (int bOn)
{
	s_bDebug = !!bOn;
}

static void LogMAC (const char *pWhat, const CMACAddress *pMAC)
{
	u8 a[MAC_ADDRESS_SIZE];
	pMAC->CopyTo (a);
	rpi_log ("%s %02x:%02x:%02x:%02x:%02x:%02x\n", pWhat, a[0], a[1], a[2], a[3], a[4], a[5]);
}

static bool FileExists (const char *pPath)
{
	FILINFO Info;
	return f_stat (pPath, &Info) == FR_OK;
}

// USB adapters are found by USB plug-and-play, possibly after rpi_net_init()
static CNetDevice *GetDevice (void)
{
	if (s_pDevice == 0 && s_Mode == NetModeUSB)
	{
		s_pDevice = CNetDevice::GetNetDevice (NetDeviceTypeEthernet);
		if (s_pDevice != 0)
		{
			LogMAC ("USB Ethernet: adapter found, MAC", s_pDevice->GetMACAddress ());
		}
	}

	return s_pDevice;
}

int rpi_net_init (const char *pMode)
{
	if (s_Mode != NetModeNone)
	{
		return 0;
	}

	TNetMode Mode;
	if (strcasecmp (pMode, "wlan") == 0 || strcasecmp (pMode, "wifi") == 0)
	{
		Mode = NetModeWLAN;
	}
	else if (strcasecmp (pMode, "usb") == 0)
	{
		Mode = NetModeUSB;
	}
	else
	{
		rpi_log ("Network: unknown setting \"ether %s\" (use wlan or usb)\n", pMode);
		return -1;
	}

	if (Mode == NetModeWLAN)
	{
		if (!FileExists (WLAN_FIRMWARE_CHECK))
		{
			rpi_log ("Wi-Fi: firmware missing (%s), network disabled\n", WLAN_FIRMWARE_CHECK);
			return -1;
		}
		if (!FileExists (WPA_CONFIG_FILE))
		{
			rpi_log ("Wi-Fi: %s missing (network name and password), network disabled\n",
				 WPA_CONFIG_FILE);
			return -1;
		}
	}

	// The WLAN driver, wpa_supplicant and Circle's USB code use tasks
	if (!CScheduler::IsActive ())
	{
		s_pScheduler = new CScheduler;
	}
	else
	{
		s_pScheduler = CScheduler::Get ();
	}

	s_pEAPOLQueue = new CNetQueue;

	if (Mode == NetModeWLAN)
	{
		rpi_log ("Wi-Fi: loading firmware...\n");
		CBcm4343Device *pWLAN = new CBcm4343Device (WLAN_FIRMWARE_PATH);
		if (!pWLAN->Initialize ())
		{
			// (not deleted: the destructor expects a running device)
			rpi_log ("Wi-Fi: cannot initialize the Wi-Fi chip, network disabled\n");
			return -1;
		}
		s_pWLAN = pWLAN;
		LogMAC ("Wi-Fi: ready, MAC", s_pWLAN->GetMACAddress ());

		s_pWPA = new CWPASupplicant (WPA_CONFIG_FILE);
		if (!s_pWPA->Initialize ())
		{
			rpi_log ("Wi-Fi: cannot start wpa_supplicant, network disabled\n");
			s_pWLAN = 0;
			return -1;
		}
		rpi_log ("Wi-Fi: connecting (see %s)...\n", WPA_CONFIG_FILE);

		s_pDevice = s_pWLAN;
	}

	s_Mode = Mode;

	if (Mode == NetModeUSB && GetDevice () == 0)
	{
		rpi_log ("USB Ethernet: no adapter found yet\n");
	}

	return 0;
}

int rpi_net_get_mac (uint8_t mac[6])
{
	CNetDevice *pDevice = GetDevice ();
	if (pDevice == 0)
	{
		return 0;
	}

	pDevice->GetMACAddress ()->CopyTo (mac);

	return 1;
}

unsigned rpi_net_poll (void)
{
	if (s_Mode == NetModeNone)
	{
		return 0;
	}

	// Called up to a few thousand times per second (every Mac interrupt and
	// idle wakeup); a USB adapter poll is a USB transfer, so limit the rate
	u64 nNow = rpi_ticks_usec ();
	if (nNow - s_nLastPoll < MIN_POLL_INTERVAL)
	{
		return RxPending ();
	}
	s_nLastPoll = nNow;

	// Let the WLAN driver, wpa_supplicant and USB tasks run
	s_pScheduler->Yield ();

	CNetDevice *pDevice = GetDevice ();
	if (pDevice == 0)
	{
		return 0;
	}

	s_nPolls++;
	if (s_bDebug && nNow - s_nLastDebug >= 5000000)
	{
		s_nLastDebug = nNow;
		rpi_log ("Network: %u polls, %u frames received, %u sent, %u not sent, %u dropped\n",
			 s_nPolls, s_nRxFrames, s_nTxFrames, s_nTxDropped, s_nRxDropped);
	}

	// USB Ethernet PHYs must be polled for link changes (every 2 s)
	if (s_Mode == NetModeUSB && nNow - s_nLastPHYUpdate >= 2000000)
	{
		s_nLastPHYUpdate = nNow;
		pDevice->UpdatePHY ();
	}

	if (nNow - s_nLastLinkCheck >= 1000000)
	{
		s_nLastLinkCheck = nNow;

		bool bLinkUp = !!pDevice->IsLinkUp ();
		if (bLinkUp != s_bLinkUp)
		{
			s_bLinkUp = bLinkUp;
			if (s_Mode == NetModeWLAN)
			{
				if (bLinkUp)
				{
					LogMAC ("Wi-Fi: connected, access point", s_pWLAN->GetBSSID ());
				}
				else
				{
					rpi_log ("Wi-Fi: disconnected\n");
				}
			}
			else
			{
				rpi_log ("USB Ethernet: link %s\n", bLinkUp ? "up" : "down");
			}
		}
	}

	// Collect received frames. Always drain the device, even when Mac OS is
	// not taking frames (early boot): the WPA key handshake must not get
	// stuck behind broadcast traffic.
	static u8 Overflow[FRAME_BUFFER_SIZE];
	for (unsigned n = 0; n < MAX_RX_PER_POLL; n++)
	{
		unsigned nNext = (s_nRxHead + 1) % RX_RING_SIZE;
		bool bRingFull = nNext == s_nRxTail;

		u8 *pFrame = bRingFull ? Overflow : s_RxRing[s_nRxHead];
		unsigned nLength;
		if (!pDevice->ReceiveFrame (pFrame, &nLength))
		{
			break;
		}

		if (nLength < ETH_HDR_LEN || nLength > ETH_MAX_FRAME + 4)
		{
			continue;
		}
		if (nLength > ETH_MAX_FRAME)
		{
			nLength = ETH_MAX_FRAME;	// trailing FCS
		}

		if (   s_Mode == NetModeWLAN
		    && pFrame[12] == (ETH_P_EAPOL >> 8)
		    && pFrame[13] == (ETH_P_EAPOL & 0xFF))
		{
			s_pEAPOLQueue->Enqueue (pFrame, nLength);
			continue;
		}

		if (bRingFull)
		{
			s_nRxDropped++;		// Mac OS is not keeping up
			continue;
		}

		s_RxLength[s_nRxHead] = nLength;
		s_nRxHead = nNext;
		s_nRxFrames++;
	}

	return RxPending ();
}

unsigned rpi_net_receive (void *pBuffer)
{
	if (s_nRxTail == s_nRxHead)
	{
		return 0;
	}

	unsigned nLength = s_RxLength[s_nRxTail];
	memcpy (pBuffer, s_RxRing[s_nRxTail], nLength);
	s_nRxTail = (s_nRxTail + 1) % RX_RING_SIZE;

	return nLength;
}

int rpi_net_send (const void *pFrame, unsigned nLength)
{
	CNetDevice *pDevice = GetDevice ();
	if (   pDevice == 0
	    || nLength < ETH_HDR_LEN
	    || nLength > ETH_MAX_FRAME
	    || !pDevice->IsLinkUp ())
	{
		s_nTxDropped++;
		return -1;
	}

	// Pad short frames (ARP etc.) to the Ethernet minimum
	u8 Padded[ETH_MIN_FRAME];
	if (nLength < ETH_MIN_FRAME)
	{
		memcpy (Padded, pFrame, nLength);
		memset (Padded + nLength, 0, ETH_MIN_FRAME - nLength);
		pFrame = Padded;
		nLength = ETH_MIN_FRAME;
	}

	if (!pDevice->SendFrame (pFrame, nLength))
	{
		s_nTxDropped++;
		return -1;
	}

	s_nTxFrames++;

	return 0;
}

void rpi_net_set_multicast (const uint8_t *pAddrs, unsigned nCount)
{
	CNetDevice *pDevice = GetDevice ();
	if (pDevice == 0)
	{
		return;
	}

	if (nCount > MAX_MULTICAST)
	{
		nCount = MAX_MULTICAST;
	}

	// The list is terminated by 00:00:00:00:00:00
	static u8 Groups[MAX_MULTICAST + 1][MAC_ADDRESS_SIZE];
	memset (Groups, 0, sizeof Groups);
	memcpy (Groups, pAddrs, nCount * MAC_ADDRESS_SIZE);

	pDevice->SetMulticastFilter (Groups);
}

int rpi_net_link_up (void)
{
	CNetDevice *pDevice = GetDevice ();

	return pDevice != 0 && pDevice->IsLinkUp ();
}

void rpi_net_log_stats (void)
{
	if (s_Mode != NetModeNone)
	{
		rpi_log ("Network: %u frames received, %u sent, %u not sent, %u dropped\n",
			 s_nRxFrames, s_nTxFrames, s_nTxDropped, s_nRxDropped);
	}
}

// ---------------------------------------------------------------------------
//  hostap l2_packet interface for wpa_supplicant (EAPOL frames only)
// ---------------------------------------------------------------------------

extern "C" {

typedef void (*eloop_sock_handler) (int sock, void *eloop_ctx, void *sock_ctx);
typedef void (*eloop_timeout_handler) (void *eloop_ctx, void *user_ctx);
int eloop_register_read_sock (int sock, eloop_sock_handler handler,
			      void *eloop_data, void *user_data);
void eloop_unregister_read_sock (int sock);
int eloop_register_timeout (unsigned int secs, unsigned int usecs,
			    eloop_timeout_handler handler,
			    void *eloop_data, void *user_data);
int eloop_cancel_timeout (eloop_timeout_handler handler,
			  void *eloop_data, void *user_data);

typedef void l2_rx_callback_t (void *ctx, const u8 *src_addr, const u8 *buf, size_t len);

struct l2_packet_data
{
	l2_rx_callback_t *rx_callback;
	void *rx_callback_ctx;
	u8 own_addr[MAC_ADDRESS_SIZE];
};

#define L2_SOCK_FD		1
#define AUTH_DURATION_SECS	5

static int s_bAuthActive = 0;

static void l2_packet_receive (int sock, void *eloop_ctx, void *sock_ctx)
{
	l2_packet_data *l2 = (l2_packet_data *) eloop_ctx;
	if (l2 == 0 || s_pEAPOLQueue == 0)
	{
		return;
	}

	static u8 Frame[FRAME_BUFFER_SIZE];
	unsigned nLength;
	while ((nLength = s_pEAPOLQueue->Dequeue (Frame)) != 0)
	{
		if (nLength > ETH_HDR_LEN)
		{
			(*l2->rx_callback) (l2->rx_callback_ctx, Frame + MAC_ADDRESS_SIZE,
					    Frame + ETH_HDR_LEN, nLength - ETH_HDR_LEN);
		}
	}
}

l2_packet_data *l2_packet_init (const char *ifname, const u8 *own_addr, unsigned short protocol,
				l2_rx_callback_t *rx_callback, void *rx_callback_ctx, int l2_hdr)
{
	if (protocol != ETH_P_EAPOL || l2_hdr != 0 || s_pWLAN == 0 || rx_callback == 0)
	{
		return 0;
	}

	l2_packet_data *l2 = (l2_packet_data *) calloc (1, sizeof *l2);
	if (l2 == 0)
	{
		return 0;
	}

	l2->rx_callback = rx_callback;
	l2->rx_callback_ctx = rx_callback_ctx;
	s_pWLAN->GetMACAddress ()->CopyTo (l2->own_addr);

	s_bAuthActive = 0;
	eloop_register_read_sock (L2_SOCK_FD, l2_packet_receive, l2, 0);

	return l2;
}

l2_packet_data *l2_packet_init_bridge (const char *br_ifname, const char *ifname,
				       const u8 *own_addr, unsigned short protocol,
				       l2_rx_callback_t *rx_callback, void *rx_callback_ctx,
				       int l2_hdr)
{
	return l2_packet_init (br_ifname, own_addr, protocol, rx_callback, rx_callback_ctx, l2_hdr);
}

void l2_packet_deinit (l2_packet_data *l2)
{
	if (l2 == 0)
	{
		return;
	}

	eloop_unregister_read_sock (L2_SOCK_FD);
	free (l2);
}

int l2_packet_get_own_addr (l2_packet_data *l2, u8 *addr)
{
	memcpy (addr, l2->own_addr, MAC_ADDRESS_SIZE);

	return 0;
}

int l2_packet_send (l2_packet_data *l2, const u8 *dst_addr, u16 proto, const u8 *buf, size_t len)
{
	if (l2 == 0 || s_pWLAN == 0 || len == 0 || len + ETH_HDR_LEN > ETH_MAX_FRAME)
	{
		return -1;
	}

	static u8 Frame[FRAME_BUFFER_SIZE];
	memcpy (Frame, dst_addr, MAC_ADDRESS_SIZE);
	memcpy (Frame + MAC_ADDRESS_SIZE, l2->own_addr, MAC_ADDRESS_SIZE);
	Frame[12] = ETH_P_EAPOL >> 8;
	Frame[13] = ETH_P_EAPOL & 0xFF;
	memcpy (Frame + ETH_HDR_LEN, buf, len);

	unsigned nLength = len + ETH_HDR_LEN;
	if (nLength < ETH_MIN_FRAME)
	{
		memset (Frame + nLength, 0, ETH_MIN_FRAME - nLength);
		nLength = ETH_MIN_FRAME;
	}

	return s_pWLAN->SendFrame (Frame, nLength) ? 0 : -1;
}

static void l2_packet_auth_end (void *eloop_ctx, void *timeout_ctx)
{
	s_bAuthActive = 0;
}

void l2_packet_notify_auth_start (l2_packet_data *l2)
{
	// wpa_supplicant polls faster while the key handshake is running
	s_bAuthActive = 1;
	eloop_cancel_timeout (l2_packet_auth_end, l2, 0);
	eloop_register_timeout (AUTH_DURATION_SECS, 0, l2_packet_auth_end, l2, 0);
}

int l2_packet_get_ip_addr (l2_packet_data *l2, char *buf, size_t len)
{
	return -1;
}

int l2_packet_set_packet_filter (l2_packet_data *l2, int type)
{
	return -1;
}

int l2_packet_auth_active (void)
{
	return s_bAuthActive;
}

}	// extern "C"
