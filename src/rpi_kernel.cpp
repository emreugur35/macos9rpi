//
// rpi_kernel.cpp - Circle (bare metal) side of macos9rpi
//
// Brings up the Raspberry Pi hardware (interrupts, timer, serial,
// framebuffer, SD card + FAT file system, USB keyboard/mouse) and then
// starts the SheepShaver PowerPC emulator core on a large private stack.
//
// macos9rpi - Mac OS 9 on bare-metal Raspberry Pi
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//

#include "rpi_glue.h"

#include <circle/startup.h>
#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/nulldevice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/serial.h>
#include <circle/usertimer.h>
#include <circle/bcmframebuffer.h>
#include <circle/chargenerator.h>
#include <circle/memory.h>
#include <circle/synchronize.h>
#include <circle/machineinfo.h>
#include <circle/memorymap.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/input/mouse.h>
#include <circle/input/console.h>
#include <circle/string.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <circle_glue.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static const char FromKernel[] = "macos9rpi";

// Default display size, if cmdline.txt does not specify width= / height=
#define DEFAULT_WIDTH	800
#define DEFAULT_HEIGHT	600

// Stack for the emulator (SheepShaver nests interpreter loops deeply)
#define EMUL_STACK_SIZE	(8 * 1024 * 1024)

extern "C" int sheepshaver_main(void);	// main_circle.cpp

// ---------------------------------------------------------------------------
//  Boot console: serial + text on the 8-bit framebuffer
// ---------------------------------------------------------------------------

enum {
	CON_BG = 0,
	CON_FG = 1,
	CON_HI = 2,
	CON_ERR = 3,
	CON_DIM = 4
};

class CBootConsole : public CDevice
{
public:
	CBootConsole (void)
	:	m_pSerial (0), m_pFB (0), m_nWidth (0), m_nHeight (0), m_nPitch (0),
		m_nCol (0), m_nRow (0), m_nColor (CON_FG)
	{
	}

	void SetSerial (CSerialDevice *pSerial)	{ m_pSerial = pSerial; }

	void Attach (u8 *pFB, unsigned nWidth, unsigned nHeight, unsigned nPitch)
	{
		m_pFB = pFB;
		m_nWidth = nWidth;
		m_nHeight = nHeight;
		m_nPitch = nPitch;
		m_nCols = nWidth / m_Font.GetCharWidth ();
		m_nRows = nHeight / m_Font.GetCharHeight ();
		m_nCol = m_nRow = 0;
		Clear ();
	}

	void Detach (void)	{ m_pFB = 0; }
	boolean IsAttached (void) const	{ return m_pFB != 0; }

	void Clear (void)
	{
		if (m_pFB == 0) return;
		for (unsigned y = 0; y < m_nHeight; y++)
			memset (m_pFB + y * m_nPitch, CON_BG, m_nWidth);
		m_nCol = m_nRow = 0;
	}

	void SetColor (u8 nColor)	{ m_nColor = nColor; }

	int Write (const void *pBuffer, size_t nCount)
	{
		if (m_pSerial)
			m_pSerial->Write (pBuffer, nCount);
		if (m_pFB)
		{
			const char *p = (const char *) pBuffer;
			for (size_t i = 0; i < nCount; i++)
				PutChar (p[i]);
		}
		return (int) nCount;
	}

private:
	void PutChar (char c)
	{
		if (c == '\n')
		{
			NewLine ();
			return;
		}
		if (c == '\r')
		{
			m_nCol = 0;
			return;
		}
		if (c == '\t')
		{
			do PutChar (' '); while (m_nCol % 8);
			return;
		}
		if ((unsigned char) c < ' ')
			return;
		if (m_nCol >= m_nCols)
			NewLine ();

		unsigned cw = m_Font.GetCharWidth (), ch = m_Font.GetCharHeight ();
		unsigned x0 = m_nCol * cw, y0 = m_nRow * ch;
		for (unsigned y = 0; y < ch; y++)
		{
			CCharGenerator::TPixelLine line = m_Font.GetPixelLine (c, y);
			u8 *pDst = m_pFB + (y0 + y) * m_nPitch + x0;
			for (unsigned x = 0; x < cw; x++)
				pDst[x] = m_Font.GetPixel (x, line) ? m_nColor : CON_BG;
		}
		m_nCol++;
	}

	void NewLine (void)
	{
		m_nCol = 0;
		if (++m_nRow >= m_nRows)
		{
			// scroll up one text line
			unsigned ch = m_Font.GetCharHeight ();
			unsigned nLines = (m_nRows - 1) * ch;
			for (unsigned y = 0; y < nLines; y++)
				memcpy (m_pFB + y * m_nPitch, m_pFB + (y + ch) * m_nPitch, m_nWidth);
			for (unsigned y = nLines; y < m_nRows * ch; y++)
				memset (m_pFB + y * m_nPitch, CON_BG, m_nWidth);
			m_nRow = m_nRows - 1;
		}
	}

private:
	CSerialDevice *m_pSerial;
	u8 *m_pFB;
	unsigned m_nWidth, m_nHeight, m_nPitch;
	unsigned m_nCols, m_nRows;
	unsigned m_nCol, m_nRow;
	u8 m_nColor;
	CCharGenerator m_Font;
};

// ---------------------------------------------------------------------------
//  Input event queue (filled in IRQ context, drained in main context)
// ---------------------------------------------------------------------------

#define INPUT_QUEUE_SIZE 256

static rpi_input_event_t s_InputQueue[INPUT_QUEUE_SIZE];
static volatile unsigned s_nInputHead = 0;	// written by producer (IRQ)
static volatile unsigned s_nInputTail = 0;	// written by consumer (main)

static void InputPut (u8 type, u8 code, int dx = 0, int dy = 0)
{
	unsigned nHead = s_nInputHead;
	unsigned nNext = (nHead + 1) % INPUT_QUEUE_SIZE;
	if (nNext == s_nInputTail)
		return;		// queue full, drop event
	rpi_input_event_t *ev = &s_InputQueue[nHead];
	ev->type = type;
	ev->code = code;
	ev->dx = (int16_t) dx;
	ev->dy = (int16_t) dy;
	DataMemBarrier ();
	s_nInputHead = nNext;
}

// USB HID usage -> raw ADB key code (see SheepShaver/src/SDL/keycodes)
// 0xFF = no mapping
static const u8 s_HIDToADB[256] =
{
	/* 0x00 */ 0xFF, 0xFF, 0xFF, 0xFF,
	/* 0x04 a..z */ 0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25,
	/* m */ 0x2E, 0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07,
	/* y z */ 0x10, 0x06,
	/* 0x1E 1..0 */ 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19, 0x1D,
	/* 0x28 Return */ 0x24,
	/* 0x29 Esc */ 0x35,
	/* 0x2A Backspace */ 0x33,
	/* 0x2B Tab */ 0x30,
	/* 0x2C Space */ 0x31,
	/* 0x2D - */ 0x1B,
	/* 0x2E = */ 0x18,
	/* 0x2F [ */ 0x21,
	/* 0x30 ] */ 0x1E,
	/* 0x31 \ */ 0x2A,
	/* 0x32 non-US # */ 0x2A,
	/* 0x33 ; */ 0x29,
	/* 0x34 ' */ 0x27,
	/* 0x35 ` */ 0x32,
	/* 0x36 , */ 0x2B,
	/* 0x37 . */ 0x2F,
	/* 0x38 / */ 0x2C,
	/* 0x39 Caps Lock */ 0x39,
	/* 0x3A F1..F12 */ 0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F,
	/* 0x46 PrintScreen (F13) */ 0x69,
	/* 0x47 Scroll Lock (F14) */ 0x6B,
	/* 0x48 Pause (F15) */ 0x71,
	/* 0x49 Insert (Help) */ 0x72,
	/* 0x4A Home */ 0x73,
	/* 0x4B Page Up */ 0x74,
	/* 0x4C Delete forward */ 0x75,
	/* 0x4D End */ 0x77,
	/* 0x4E Page Down */ 0x79,
	/* 0x4F Right */ 0x3C,
	/* 0x50 Left */ 0x3B,
	/* 0x51 Down */ 0x3D,
	/* 0x52 Up */ 0x3E,
	/* 0x53 Num Lock (Clear) */ 0x47,
	/* 0x54 KP / */ 0x4B,
	/* 0x55 KP * */ 0x43,
	/* 0x56 KP - */ 0x4E,
	/* 0x57 KP + */ 0x45,
	/* 0x58 KP Enter */ 0x4C,
	/* 0x59 KP 1..9 */ 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C,
	/* 0x62 KP 0 */ 0x52,
	/* 0x63 KP . */ 0x41,
	/* 0x64 non-US \ (ISO key) */ 0x0A,
	/* 0x65 Application (Menu) */ 0x32,
	/* 0x66 Power */ 0x7F,
	/* 0x67 KP = */ 0x51,
	/* 0x68 F13..F15 */ 0x69, 0x6B, 0x71,
	// rest: 0
};

static bool s_bSwapOptCmd = false;
static bool s_bCapsLocked = false;

static u8 ModifierToADB (unsigned nBit)
{
	switch (nBit)
	{
	case 0: return 0x36;					// Left Ctrl  -> Control
	case 1: return 0x38;					// Left Shift -> Shift
	case 2: return s_bSwapOptCmd ? 0x3A : 0x37;	// Left Alt   -> Command
	case 3: return s_bSwapOptCmd ? 0x37 : 0x3A;	// Left GUI   -> Option
	case 4: return 0x36;					// Right Ctrl
	case 5: return 0x38;					// Right Shift
	case 6: return s_bSwapOptCmd ? 0x3A : 0x37;	// Right Alt  -> Command
	case 7: return s_bSwapOptCmd ? 0x37 : 0x3A;	// Right GUI  -> Option
	}
	return 0xFF;
}

static u8 HIDToADB (u8 nUsage)
{
	u8 nCode = s_HIDToADB[nUsage];
	if (nCode == 0 && nUsage != 0x04)	// table is zero-filled beyond its end
		return 0xFF;
	return nCode;
}

static void KeyEvent (u8 nUsage, bool bDown)
{
	if (nUsage == 0x39)		// Caps Lock is a locking key on Mac keyboards
	{
		if (!bDown)
			return;
		s_bCapsLocked = !s_bCapsLocked;
		InputPut (s_bCapsLocked ? RPI_EV_KEY_DOWN : RPI_EV_KEY_UP, 0x39);
		return;
	}
	u8 nCode = HIDToADB (nUsage);
	if (nCode == 0xFF)
		return;
	InputPut (bDown ? RPI_EV_KEY_DOWN : RPI_EV_KEY_UP, nCode);
}

static unsigned char s_ucLastModifiers = 0;
static unsigned char s_LastKeys[6] = {0};

static void KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	// Ignore "phantom" state (keyboard rollover error)
	if (RawKeys[0] == 0x01)
		return;

	// Modifiers
	unsigned char ucChanged = ucModifiers ^ s_ucLastModifiers;
	for (unsigned i = 0; i < 8; i++)
	{
		if (ucChanged & (1 << i))
		{
			// Left and right variants share one ADB code: only send key up
			// if the other side is not pressed
			bool bDown = (ucModifiers >> i) & 1;
			unsigned nOther = i ^ 4;
			bool bOtherDown = (ucModifiers >> nOther) & 1;
			if (!bDown && bOtherDown && ModifierToADB (i) == ModifierToADB (nOther))
				continue;
			InputPut (bDown ? RPI_EV_KEY_DOWN : RPI_EV_KEY_UP, ModifierToADB (i));
		}
	}
	s_ucLastModifiers = ucModifiers;

	// Released keys
	for (unsigned i = 0; i < 6; i++)
	{
		u8 k = s_LastKeys[i];
		if (k < 4)
			continue;
		bool bStill = false;
		for (unsigned j = 0; j < 6; j++)
			if (RawKeys[j] == k)
				bStill = true;
		if (!bStill)
			KeyEvent (k, false);
	}
	// Pressed keys
	for (unsigned i = 0; i < 6; i++)
	{
		u8 k = RawKeys[i];
		if (k < 4)
			continue;
		bool bWas = false;
		for (unsigned j = 0; j < 6; j++)
			if (s_LastKeys[j] == k)
				bWas = true;
		if (!bWas)
			KeyEvent (k, true);
	}
	memcpy (s_LastKeys, RawKeys, 6);
}

static unsigned s_nLastButtons = 0;

static volatile unsigned s_nMouseReports = 0;
static volatile int s_nMouseLastDX = 0, s_nMouseLastDY = 0;

extern "C" unsigned rpi_mouse_debug (int *pDX, int *pDY)
{
	*pDX = s_nMouseLastDX;
	*pDY = s_nMouseLastDY;
	return s_nMouseReports;
}

static void MouseStatusHandler (unsigned nButtons, int nDX, int nDY, int nWheel)
{
	s_nMouseReports++;
	if (nDX || nDY)
	{
		s_nMouseLastDX = nDX;
		s_nMouseLastDY = nDY;
	}

	if (nDX != 0 || nDY != 0)
		InputPut (RPI_EV_MOUSE_MOVE, 0, nDX, nDY);

	static const unsigned ButtonMask[3] = {MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE};
	for (unsigned i = 0; i < 3; i++)
	{
		unsigned m = ButtonMask[i];
		if ((nButtons ^ s_nLastButtons) & m)
			InputPut ((nButtons & m) ? RPI_EV_MOUSE_DOWN : RPI_EV_MOUSE_UP, i);
	}
	s_nLastButtons = nButtons;

	if (nWheel != 0)
		InputPut (RPI_EV_MOUSE_WHEEL, 0, 0, nWheel);
}

// ---------------------------------------------------------------------------
//  The kernel object
// ---------------------------------------------------------------------------

class CMacKernel
{
public:
	CMacKernel (void)
	:	m_Timer (&m_Interrupt),
		m_Logger (LogNotice, &m_Timer),
		m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED),
		m_USBHCI (&m_Interrupt, &m_Timer, TRUE),
		m_Console (&m_NullDevice, &m_BootConsole),
		m_pFrameBuffer (0),
		m_pUserTimer (0),
		m_pTickHandler (0),
		m_nTickPeriod (0),
		m_pKeyboard (0),
		m_pMouse (0)
	{
		s_pThis = this;
	}

	bool Initialize (void);
	void Run (void);

	bool InitFrameBuffer (unsigned nWidth, unsigned nHeight, unsigned nDepth);
	void SetBootPalette (void);
	void UpdateUSB (void);

	static void UserTimerHandler (CUserTimer *pUserTimer, void *pParam);
	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);
	static void MouseRemovedHandler (CDevice *pDevice, void *pContext);

public:
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CNullDevice		m_NullDevice;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CSerialDevice		m_Serial;
	CBootConsole		m_BootConsole;
	CLogger			m_Logger;
	CEMMCDevice		m_EMMC;
	FATFS			m_FileSystem;
	CUSBHCIDevice		m_USBHCI;
	CConsole		m_Console;

	CBcmFrameBuffer	       *m_pFrameBuffer;
	CUserTimer	       *m_pUserTimer;
	rpi_tick_handler_t	m_pTickHandler;
	unsigned		m_nTickPeriod;

	CUSBKeyboardDevice     *m_pKeyboard;
	CMouseDevice	       *m_pMouse;

	static CMacKernel      *s_pThis;
};

CMacKernel *CMacKernel::s_pThis = 0;

bool CMacKernel::InitFrameBuffer (unsigned nWidth, unsigned nHeight, unsigned nDepth)
{
	if (m_pFrameBuffer != 0)
	{
		if (   m_pFrameBuffer->GetWidth () == nWidth
		    && m_pFrameBuffer->GetHeight () == nHeight
		    && m_pFrameBuffer->GetDepth () == nDepth)
			return true;

		delete m_pFrameBuffer;
		m_pFrameBuffer = 0;
	}

	m_pFrameBuffer = new CBcmFrameBuffer (nWidth, nHeight, nDepth);
	if (!m_pFrameBuffer->Initialize ())
	{
		delete m_pFrameBuffer;
		m_pFrameBuffer = 0;
		return false;
	}
	return true;
}

void CMacKernel::SetBootPalette (void)
{
	if (m_pFrameBuffer == 0)
		return;
	m_pFrameBuffer->SetPalette32 (CON_BG,  0xFF402020);	// dark blue-gray (ABGR)
	m_pFrameBuffer->SetPalette32 (CON_FG,  0xFFE0E0E0);	// light gray
	m_pFrameBuffer->SetPalette32 (CON_HI,  0xFF40E0FF);	// yellow
	m_pFrameBuffer->SetPalette32 (CON_ERR, 0xFF4040FF);	// red
	m_pFrameBuffer->SetPalette32 (CON_DIM, 0xFFA08080);	// dim
	m_pFrameBuffer->UpdatePalette ();
}

bool CMacKernel::Initialize (void)
{
	if (!m_Interrupt.Initialize ())
		return false;

	if (!m_Serial.Initialize (115200))
		return false;
	m_BootConsole.SetSerial (&m_Serial);

	// Framebuffer (8 bit, palettized: Mac OS draws directly into it)
	unsigned nWidth = m_Options.GetWidth ();
	unsigned nHeight = m_Options.GetHeight ();
	if (nWidth == 0 || nHeight == 0)
	{
		nWidth = DEFAULT_WIDTH;
		nHeight = DEFAULT_HEIGHT;
	}
	if (   InitFrameBuffer (nWidth, nHeight, 8)
	    || InitFrameBuffer (640, 480, 8))
	{
		SetBootPalette ();
		m_BootConsole.Attach ((u8 *) (uintptr) m_pFrameBuffer->GetBuffer (),
				      m_pFrameBuffer->GetWidth (), m_pFrameBuffer->GetHeight (),
				      m_pFrameBuffer->GetPitch ());
	}

	if (!m_Logger.Initialize (&m_BootConsole))
		return false;

	if (!m_Timer.Initialize ())
		return false;

	m_Console.Initialize ();
	CGlueStdioInit (m_Console);

	m_BootConsole.SetColor (CON_HI);
	printf ("macos9rpi - Mac OS 9 on bare-metal Raspberry Pi\n");
	m_BootConsole.SetColor (CON_FG);
	printf ("Running on %s, %u MB ARM memory, display %ux%u\n",
		CMachineInfo::Get ()->GetMachineName (),
		(unsigned) (CMemorySystem::Get ()->GetMemSize () >> 20),
		m_pFrameBuffer ? m_pFrameBuffer->GetWidth () : 0,
		m_pFrameBuffer ? m_pFrameBuffer->GetHeight () : 0);
	printf ("Built " __DATE__ " " __TIME__ "\n\n");

	if (!m_EMMC.Initialize ())
	{
		rpi_fatal ("Cannot initialize the SD card.");
	}

	if (f_mount (&m_FileSystem, "SD:", 1) != FR_OK)
	{
		rpi_fatal ("Cannot mount the FAT partition of the SD card.");
	}
	f_chdrive ("SD:");

	if (!m_USBHCI.Initialize ())
	{
		printf ("WARNING: Cannot initialize USB host controller\n");
	}

	return true;
}

void CMacKernel::UpdateUSB (void)
{
	if (m_USBHCI.UpdatePlugAndPlay ())
	{
		CDeviceNameService *pDNS = CDeviceNameService::Get ();

		if (m_pKeyboard == 0)
		{
			m_pKeyboard = (CUSBKeyboardDevice *) pDNS->GetDevice ("ukbd1", FALSE);
			if (m_pKeyboard != 0)
			{
				m_pKeyboard->RegisterRemovedHandler (KeyboardRemovedHandler);
				m_pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandlerRaw);
				printf ("USB keyboard attached\n");
			}
		}

		if (m_pMouse == 0)
		{
			m_pMouse = (CMouseDevice *) pDNS->GetDevice ("mouse1", FALSE);
			if (m_pMouse != 0)
			{
				m_pMouse->RegisterRemovedHandler (MouseRemovedHandler);
				m_pMouse->RegisterStatusHandler (MouseStatusHandler);
				printf ("USB mouse attached\n");
			}
		}
	}
}

void CMacKernel::KeyboardRemovedHandler (CDevice *pDevice, void *pContext)
{
	s_pThis->m_pKeyboard = 0;
	memset (s_LastKeys, 0, sizeof s_LastKeys);
	s_ucLastModifiers = 0;
}

void CMacKernel::MouseRemovedHandler (CDevice *pDevice, void *pContext)
{
	s_pThis->m_pMouse = 0;
	s_nLastButtons = 0;
}

// One hardware timer (system timer C1) serves two deadlines: the periodic
// 60 Hz tick and the next Mac Time Manager wakeup (precise timing).

#define TM_NEVER	0x7FFFFFFFFFFFFFFFLL
#define MIN_DELAY	20		// microseconds

static volatile u64 s_nNextTick = 0;
static volatile s64 s_nTMWakeup = TM_NEVER;
static volatile u64 s_nTimerTarget = 0;
static rpi_tick_handler_t s_pTMHandler = 0;

static void ProgramTimer (CUserTimer *pUserTimer, u64 nNow)
{
	u64 nTarget = s_nNextTick;
	if (s_nTMWakeup != TM_NEVER && (u64) s_nTMWakeup < nTarget)
		nTarget = (u64) s_nTMWakeup;
	s64 nDelay = (s64) (nTarget - nNow);
	if (nDelay < MIN_DELAY)
		nDelay = MIN_DELAY;
	s_nTimerTarget = nNow + nDelay;
	pUserTimer->Start ((unsigned) nDelay);
}

void CMacKernel::UserTimerHandler (CUserTimer *pUserTimer, void *pParam)
{
	CMacKernel *pThis = (CMacKernel *) pParam;
	u64 nNow = CTimer::GetClockTicks64 ();

	bool bTick = false;
	if (nNow >= s_nNextTick)
	{
		s_nNextTick += pThis->m_nTickPeriod;
		if (s_nNextTick <= nNow)			// fell behind
			s_nNextTick = nNow + pThis->m_nTickPeriod;
		bTick = true;
	}

	bool bTM = false;
	if (s_nTMWakeup != TM_NEVER && (u64) s_nTMWakeup <= nNow)
	{
		s_nTMWakeup = TM_NEVER;
		bTM = true;
	}

	ProgramTimer (pUserTimer, nNow);

	if (bTick && pThis->m_pTickHandler)
		(*pThis->m_pTickHandler) ();
	if (bTM && s_pTMHandler)
		(*s_pTMHandler) ();
}

// Switch to a big private stack and run the emulator
extern "C" void rpi_call_on_stack (int (*pFunc) (void), void *pStackTop);	// rpi_abort_stub.S

static int RunEmulator (void)
{
	return sheepshaver_main ();
}

void CMacKernel::Run (void)
{
	u8 *pStack = (u8 *) malloc (EMUL_STACK_SIZE);
	if (pStack == 0)
		rpi_fatal ("Not enough memory for emulator stack");
	void *pStackTop = (void *) (((uintptr) pStack + EMUL_STACK_SIZE) & ~15U);
	rpi_call_on_stack (RunEmulator, pStackTop);
}

// ---------------------------------------------------------------------------
//  C glue implementation
// ---------------------------------------------------------------------------

#define K CMacKernel::s_pThis

extern "C" {

static void ReattachConsole (void);

static char s_LogBuffer[2048];
static char s_LogCapture[32768];
static unsigned s_LogCaptureLen;

void rpi_log (const char *fmt, ...)
{
	va_list ap;
	va_start (ap, fmt);
	int n = vsnprintf (s_LogBuffer, sizeof s_LogBuffer, fmt, ap);
	va_end (ap);
	if (n < 0)
		return;
	if ((size_t) n >= sizeof s_LogBuffer)
		n = sizeof s_LogBuffer - 1;
	K->m_BootConsole.Write (s_LogBuffer, n);

	// Keep a copy for macos9rpi.log (rpi_log_save)
	if (s_LogCaptureLen + n <= sizeof s_LogCapture)
	{
		memcpy (s_LogCapture + s_LogCaptureLen, s_LogBuffer, n);
		s_LogCaptureLen += n;
	}
}

// Write the messages so far to macos9rpi.log on the FAT partition, so they
// can be read without a serial cable
void rpi_log_save (void)
{
	FIL File;
	if (f_open (&File, "SD:/macos9rpi.log", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		return;
	UINT nWritten;
	f_write (&File, s_LogCapture, s_LogCaptureLen, &nWritten);
	f_close (&File);
}

void rpi_console_detach (void)
{
	K->m_BootConsole.Detach ();
}

static void ReattachConsole (void)
{
	if (!K->m_BootConsole.IsAttached () && K->m_pFrameBuffer != 0
	    && K->m_pFrameBuffer->GetDepth () == 8)
	{
		K->SetBootPalette ();
		K->m_BootConsole.Attach ((u8 *) (uintptr) K->m_pFrameBuffer->GetBuffer (),
					 K->m_pFrameBuffer->GetWidth (),
					 K->m_pFrameBuffer->GetHeight (),
					 K->m_pFrameBuffer->GetPitch ());
	}
}

void rpi_show_shutdown_screen (void)
{
	rpi_tick_stop ();
	ReattachConsole ();
	K->m_BootConsole.SetColor (CON_HI);
	const char *pMsg = "\nMac OS has shut down.\n\n";
	K->m_BootConsole.Write (pMsg, strlen (pMsg));
	K->m_BootConsole.SetColor (CON_FG);
	pMsg = "It is now safe to switch off the Raspberry Pi.\n";
	K->m_BootConsole.Write (pMsg, strlen (pMsg));
}

void rpi_fatal (const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start (ap, fmt);
	vsnprintf (buf, sizeof buf, fmt, ap);
	va_end (ap);

	rpi_tick_stop ();

	// Bring back a readable console
	ReattachConsole ();

	K->m_BootConsole.SetColor (CON_ERR);
	K->m_BootConsole.Write ("\nFATAL: ", 8);
	K->m_BootConsole.Write (buf, strlen (buf));
	K->m_BootConsole.Write ("\n", 1);
	K->m_BootConsole.SetColor (CON_FG);
	const char *pMsg = "\nSystem halted. Check the SD card contents and macos9rpi.txt, then power cycle.\n";
	K->m_BootConsole.Write (pMsg, strlen (pMsg));

	// Blink the activity LED forever
	for (;;)
	{
		K->m_ActLED.On ();
		CTimer::SimpleMsDelay (200);
		K->m_ActLED.Off ();
		CTimer::SimpleMsDelay (800);
	}
}

uint64_t rpi_ticks_usec (void)
{
	return CTimer::GetClockTicks64 ();
}

void rpi_delay_usec (uint64_t usec)
{
	if (usec == 0)
		return;
	uint64_t nEnd = CTimer::GetClockTicks64 () + usec;
	while (CTimer::GetClockTicks64 () < nEnd)
	{
		if (nEnd - CTimer::GetClockTicks64 () > 2000)
			asm volatile ("mcr p15, 0, %0, c7, c0, 4" : : "r" (0));	// WFI, the timer IRQ (HZ=100) wakes us up
	}
}

// Parse __DATE__ ("Nov  3 2026") into a Unix time stamp
static uint32_t BuildTime (void)
{
	static const char Months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const char *pDate = __DATE__;
	int nMonth = 0;
	for (int i = 0; i < 12; i++)
		if (strncmp (pDate, Months + i * 3, 3) == 0)
			nMonth = i;
	int nDay = atoi (pDate + 4);
	int nYear = atoi (pDate + 7);

	static const int DaysBefore[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	uint32_t nDays = (nYear - 1970) * 365 + (nYear - 1969) / 4 + DaysBefore[nMonth] + nDay - 1;
	if (nMonth > 1 && (nYear % 4) == 0)
		nDays++;
	return nDays * 86400 + 12 * 3600;
}

uint32_t rpi_unix_time (void)
{
	static uint32_t s_nBase = 0;
	unsigned nSeconds, nMicros;
	if (CTimer::Get ()->GetUniversalTime (&nSeconds, &nMicros) && nSeconds > 1000000000)
		return nSeconds;

	// No RTC: count from the build time
	if (s_nBase == 0)
		s_nBase = BuildTime ();
	return s_nBase + (uint32_t) (CTimer::GetClockTicks64 () / 1000000);
}

void *rpi_alloc_1mb_aligned (uint32_t size)
{
	u8 *p = (u8 *) malloc (size + MEGABYTE);
	if (p == 0)
		return 0;
	return (void *) (((uintptr) p + MEGABYTE - 1) & ~(MEGABYTE - 1));
}

uint32_t rpi_arm_memory_size (void)
{
	return (uint32_t) CMemorySystem::Get ()->GetMemSize ();
}

const char *rpi_sd_root (void)
{
	return "SD:/";
}

int rpi_sd_read (uint64_t offset, void *buf, uint32_t len)
{
	if (K->m_EMMC.Seek (offset) != offset)
		return -1;
	return K->m_EMMC.Read (buf, len);
}

int rpi_sd_write (uint64_t offset, const void *buf, uint32_t len)
{
	if (K->m_EMMC.Seek (offset) != offset)
		return -1;
	return K->m_EMMC.Write (buf, len);
}

uint64_t rpi_sd_size (void)
{
	return K->m_EMMC.GetSize ();
}

// ---- Framebuffer -----------------------------------------------------------

uint8_t *rpi_fb_init (unsigned width, unsigned height, unsigned depth,
		      unsigned *out_width, unsigned *out_height, unsigned *out_pitch)
{
	bool bWasAttached = K->m_BootConsole.IsAttached ();
	K->m_BootConsole.Detach ();
	if (!K->InitFrameBuffer (width, height, depth))
		return 0;
	*out_width = K->m_pFrameBuffer->GetWidth ();
	*out_height = K->m_pFrameBuffer->GetHeight ();
	*out_pitch = K->m_pFrameBuffer->GetPitch ();
	uint8_t *pBuffer = (uint8_t *) (uintptr) K->m_pFrameBuffer->GetBuffer ();
	if (bWasAttached && depth == 8)
	{
		K->SetBootPalette ();
		K->m_BootConsole.Attach (pBuffer, *out_width, *out_height, *out_pitch);
	}
	return pBuffer;
}

void rpi_fb_set_palette (unsigned first, unsigned count, const uint8_t *rgb)
{
	if (K->m_pFrameBuffer == 0 || K->m_pFrameBuffer->GetDepth () > 8)
		return;
	for (unsigned i = 0; i < count && first + i < 256; i++, rgb += 3)
	{
		u32 nRGBA = (u32) rgb[0] | ((u32) rgb[1] << 8) | ((u32) rgb[2] << 16) | 0xFF000000;
		K->m_pFrameBuffer->SetPalette32 (first + i, nRGBA);
	}
	K->m_pFrameBuffer->UpdatePalette ();
}

void rpi_fb_preferred_size (unsigned *width, unsigned *height)
{
	*width = K->m_Options.GetWidth ();
	*height = K->m_Options.GetHeight ();
	if (*width == 0 || *height == 0)
	{
		*width = DEFAULT_WIDTH;
		*height = DEFAULT_HEIGHT;
	}
}

// ---- Tick ---------------------------------------------------------------------

int rpi_tick_start (rpi_tick_handler_t handler, unsigned period_usec)
{
	K->m_pTickHandler = handler;
	K->m_nTickPeriod = period_usec;
	if (K->m_pUserTimer == 0)
	{
		K->m_pUserTimer = new CUserTimer (&K->m_Interrupt, CMacKernel::UserTimerHandler, K);
		if (!K->m_pUserTimer->Initialize ())
			return 0;
	}
	EnterCritical ();
	u64 nNow = CTimer::GetClockTicks64 ();
	s_nNextTick = nNow + period_usec;
	ProgramTimer (K->m_pUserTimer, nNow);
	LeaveCritical ();
	return 1;
}

void rpi_tm_set_handler (rpi_tick_handler_t handler)
{
	s_pTMHandler = handler;
}

// Set the time (rpi_ticks_usec timeline) of the next Time Manager wakeup
void rpi_tm_set_wakeup (int64_t wakeup)
{
	if (K->m_pUserTimer == 0)
	{
		s_nTMWakeup = wakeup;
		return;
	}
	EnterCritical ();
	s_nTMWakeup = wakeup;
	if (wakeup != TM_NEVER && (u64) wakeup < s_nTimerTarget)
		ProgramTimer (K->m_pUserTimer, CTimer::GetClockTicks64 ());
	LeaveCritical ();
}

void rpi_tick_stop (void)
{
	if (K == 0)
		return;
	K->m_pTickHandler = 0;
}

// ---- Input ----------------------------------------------------------------------

int rpi_input_get (rpi_input_event_t *ev)
{
	unsigned nTail = s_nInputTail;
	if (nTail == s_nInputHead)
		return 0;
	DataMemBarrier ();
	*ev = s_InputQueue[nTail];
	s_nInputTail = (nTail + 1) % INPUT_QUEUE_SIZE;
	return 1;
}

void rpi_usb_poll (void)
{
	K->UpdateUSB ();
}

int rpi_have_keyboard (void)	{ return K->m_pKeyboard != 0; }
int rpi_have_mouse (void)	{ return K->m_pMouse != 0; }

void rpi_set_swap_opt_cmd (int swap)	{ s_bSwapOptCmd = swap != 0; }

// ---- Power --------------------------------------------------------------------------

void rpi_halt (void)
{
	halt ();
	for (;;) ;
}

void rpi_reboot (void)
{
	reboot ();
	for (;;) ;
}

} // extern "C"

// ---------------------------------------------------------------------------
//  Entry point
// ---------------------------------------------------------------------------

int main (void)
{
	CMacKernel Kernel;
	if (!Kernel.Initialize ())
	{
		halt ();
		return EXIT_HALT;
	}

	Kernel.Run ();

	// The emulator returned (Mac OS shut down)
	printf ("\nMac OS has shut down. You can switch off the Raspberry Pi now.\n");
	halt ();
	return EXIT_HALT;
}
