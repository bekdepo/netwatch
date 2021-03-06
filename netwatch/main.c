/* main.c
 * Main post-paging entry point.  Actually, this is a lie.
 * NetWatch system management mode administration console
 *
 * Copyright (c) 2008 Jacob Potter and Joshua Wise.  All rights reserved.
 * This program is free software; you can redistribute and/or modify it under
 * the terms found in the file LICENSE in the root of this source tree.
 *
 */

#include <io.h>
#include <smram.h>
#include <video_defines.h>
#include <minilib.h>
#include <smi.h>
#include <pci-bother.h>
#include <fb.h>
#include <output.h>
#include "../net/net.h"
#include "vga-overlay.h"
#include "packet.h"
#include "keyboard.h"

unsigned int lastctr = 0;
extern unsigned int counter;

static int _ibf_ready = 0;
static int _waiting_for_data = 0;
static int curdev = 0;
static int adding_locks_from_time_to_time = 0;

static int _inject_ready()
{
	return _ibf_ready && !_waiting_for_data && !adding_locks_from_time_to_time;
}

void _try_inject()
{
	if (kbd_has_injected_scancode() && _inject_ready())
	{
		smi_disable_event(SMI_EVENT_DEVTRAP_KBC);
		int i = 1000;
		while (inb(0x64) & 0x02)	/* Busy? */
			;
		outb(0x64, 0xD2);	/* "Inject, please!" */
		while (inb(0x64) & 0x02)	/* Busy? */
			;
		outb(0x60, kbd_get_injected_scancode());	/* data */
		while ((inb(0x64) & 0x02) && i--)	/* wait for completion */
			;
		/* On some chipsets, this might set the "device active" bit
		 * for the keyboard controller.  On ICH2, we appear to get
		 * lucky, but we need a mechanism of saying "I just touched
		 * the keyboard, please don't send me another SMI because of
		 * this"... XXX
		 * ICH2: outl(0x844, 0x1000);
		 */
		adding_locks_from_time_to_time++;
		smi_enable_event(SMI_EVENT_DEVTRAP_KBC);
	} else if (kbd_has_injected_scancode())
		outputf("Would like to inject, but %d %d", _ibf_ready, _waiting_for_data);
}

void pci_dump() {
	unsigned long cts;
		
	cts = inl(0x84C);
	
	outl(0x840, 0x0);
	outl(0x848, 0x0);
	switch(cts&0xF0000)
	{
	case 0x20000:
	{
		unsigned char b;
		
		switch (cts & 0xFFFF)
		{
		case 0x64:
			/* Read the real hardware and mask in our OBF if need be. */
			b = inb(0x64);
			
			curdev = (b & 0x20 /* KBD_STAT_MOUSE_OBF */) ? 1 : 0;
			
			if ((curdev == 0) && kbd_has_injected_scancode())
				b |= 0x1;

			_ibf_ready = (b & 0x2 /* IBF */) ? 0 : 1;
			
			break;
		case 0x60:
			if ((curdev == 0) && kbd_has_injected_scancode() && !adding_locks_from_time_to_time)
				b = kbd_get_injected_scancode();
			else
				b = inb(0x60);
			if (adding_locks_from_time_to_time)
				adding_locks_from_time_to_time--;
			if ((curdev == 0) && (b == 0x01)) {	/* Escape */
				outb(0xCF9, 0x4);	/* Reboot */
				return;
			}
			break;

		default:
			b = inb(cts & 0xFFFF);
		}
		
		dologf("READ : %08x (%02x)", cts, b);
		*(unsigned char*)0xAFFD0 /* EAX */ = b;
		break;
	}
	case 0x30000:
	{
		unsigned char b;
		
		b = *(unsigned char*)0xAFFD0 /* EAX */;
		dologf("WRITE: %08x (%02x)", cts, b);
		if ((cts & 0xFFFF) == 0x64)
			switch(b)
			{
			case 0x60 /*KBD_CCMD_WRITE_MODE*/:
			case 0xD2 /*KBD_CCMD_WRITE_OBUF*/:
			case 0xD3 /*KBD_CCMD_WRITE_AUX_OBUF*/:
			case 0xD4 /*KBD_CCMD_WRITE_MOUSE*/:
			case 0xD1 /*KBD_CCMD_WRITE_OUTPORT*/:
				_waiting_for_data = 1;	/* These all should not be interrupted. */
			}
		else if ((cts & 0xFFFF) == 0x60)
			_waiting_for_data = 0;
		outb(cts & 0xFFFF, b);
		break;
	}
	default:
		dolog("Unhandled PCI cycle");
	}
	
	outl(0x840, 0x0);
	outl(0x844, 0x1000);
	outl(0x848, 0x1000);
}

void timer_handler(smi_event_t ev)
{
	static unsigned int ticks = 0;
	
	smi_disable_event(SMI_EVENT_FAST_TIMER);
	smi_enable_event(SMI_EVENT_FAST_TIMER);
	
	_try_inject();
	
	outb(0x80, (ticks++) & 0xFF);
	
	if (!fb || fb->curmode.text)
		outlog();
}

void kbc_handler(smi_event_t ev)
{
	pci_dump();
}

void gbl_rls_handler(smi_event_t ev)
{
	unsigned long ecx;
	
	ecx = *(unsigned long*)0xAFFD4;

	packet_t * packet = check_packet(ecx);
	if (!packet)
	{
		dologf("WARN: bad packet at %08x", ecx);
		return;
	}

	outputf("Got packet: type %08x", packet->type);

	if (packet->type == 42) {
		dump_log((char *)packet->data);
		*(unsigned long*)0xAFFD4 = 42;
	} else if (packet->type == 0xAA) {
		kbd_inject_keysym('A', 1);
		kbd_inject_keysym('A', 0);
	} else {
		*(unsigned long*)0xAFFD4 = 0x2BADD00D;
	}
}
