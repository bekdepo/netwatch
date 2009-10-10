/* main.c
 * Main program for starting SMM code
 * NetWatch multiboot loader
 *
 * Copyright (c) 2008 Jacob Potter and Joshua Wise.  All rights reserved.
 * This program is free software; you can redistribute and/or modify it under
 * the terms found in the file LICENSE in the root of this source tree. 
 *
 */

#include "console.h"
#include "loader.h"
#include <output.h>
#include <minilib.h>
#include <io.h>
#include <smram.h>
#include <multiboot.h>
#include <smi.h>
#include <pci.h>

#define INFO_SIGNATURE 0x5754454E

extern char _binary_realmode_bin_start[];
extern int _binary_realmode_bin_size;

struct info_section
{
	unsigned int signature;
	void (*firstrun)();
};

void panic(const char *msg)
{
	outputf("PANIC: %s\nSystem halted\n", msg);
	while(1) { __asm__("hlt"); }
}

void c_start(unsigned int magic, struct mb_info *mbinfo)
{
	struct mod_info *mods = mbinfo->mods;
	smram_state_t old_smramc;
	struct info_section * info;
	int i;
	
	void (*realmode)() = (void (*)()) 0x4000;
	
	show_cursor();
	outputf("NetWatch loader");
	
	if (magic != MULTIBOOT_LOADER_MAGIC)
		panic("Bootloader was not multiboot compliant; cannot continue.");
	
	for (i = 0; i < mbinfo->mod_cnt; i++)
	{
		outputf("Module found:");
		outputf("  Start: %08x", (unsigned long) mods[i].mod_start);
		outputf("  Size: %08x", (unsigned long)mods[i].mod_end - (unsigned long)mods[i].mod_start);
		outputf("  Name: %s", mods[i].mod_string);
	}

	if (mbinfo->mod_cnt != 1)
		panic("Expected exactly one module; cannot continue.");
	outputf("Current SMRAMC state is: %02x", (unsigned char)smram_save_state());
	outputf("Current SMI state is: %08x", inl(0x830));	// XXX ICH2 specific
	
	smi_disable();
	
	/* Open the SMRAM aperture and load our ELF. */
	old_smramc = smram_save_state();

	if (smram_aseg_set_state(SMRAM_ASEG_OPEN) != 0)
		panic("Opening SMRAM failed; cannot load ELF.");

	load_elf(mods[0].mod_start, (unsigned long)mods[0].mod_end - (unsigned long)mods[0].mod_start);

	info = (struct info_section *)0x10000;
	if (info->signature != INFO_SIGNATURE)
	{
		smram_restore_state(old_smramc);		/* Restore so that video ram is touchable again. */
		panic("Info section signature mismatch.");
	}

	info->firstrun();
	smram_restore_state(old_smramc);
	
	outputf("New SMRAMC state is: %02x", (unsigned char)smram_save_state());

	puts("Waiting for a bit before returning to real mode...");
	for (i=0; i<0x500000; i++)
	{
		if ((i % 0x100000) == 0)
			puts(".");
		inb(0x80);
	}
	puts("\n");

	outputf("Now returning to real mode.");
	memcpy((void *)0x4000, _binary_realmode_bin_start, (int)&_binary_realmode_bin_size);
	realmode();	// goodbye!
}
