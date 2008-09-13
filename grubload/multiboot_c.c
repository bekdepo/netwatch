#include "console.h"
#include <io.h>
#include <smram.h>

extern char _binary_realmode_bin_start[];
extern int _binary_realmode_bin_size;

struct mb_info
{
	unsigned long flags;
	unsigned long mem_lower, mem_upper;
	unsigned long boot_dev;
	char *cmdline;
	unsigned long mod_cnt;
	struct mod_info *mods;
};

struct mod_info
{
	void *mod_start;
	void *mod_end;
	char *mod_string;
	void *reserved;
};

void c_start(unsigned int magic, struct mb_info *info)
{
	struct mod_info *mods = mods;
	unsigned short *grubptr = (unsigned short *)0x7CFE;
	unsigned char smramc;
	int i;
	
	void (*realmode)() = (void (*)()) 0x4000;
	
	show_cursor();
	puts("NetWatch loader\n");
	
	if (magic != 0x2BADB002)
	{
		puts("Bootloader was not multiboot compliant; cannot continue.\n");
		while(1) asm("hlt");
	}
	
	for (i = 0; i < info->mod_cnt; i++)
	{
		puts("Module found:\n");
		puts("  Start: "); puthex(mods[i].mod_start); puts("\n");
		puts("  Size: "); puthex(mods[i].mod_end - mods[i].mod_start); puts("\n");
		puts("  Name: "); puts(mods[i].mod_string); puts("\n");
	}

	if (info->mod_cnt != 1)
	{
		puts("Expected exactly one module; cannot continue.\n");
		while(1) asm("hlt");
	}

	puts("Current USB state is: "); puthex(pci_read16(0, 31, 2, 0xC0)); puts(" "); puthex(pci_read16(0, 31, 4, 0xC0)); puts("\n");
	puts("Current SMI state is: "); puthex(inl(0x830)); puts("\n");
	puts("Current SMRAMC state is: "); puthex(pci_read8(0, 0, 0, 0x70)); puts("\n");
	
	outl(0x830, inl(0x830) & ~0x1);	/* turn off SMIs */
	
	/* Try really hard to shut up USB_LEGKEY. */
	pci_write16(0, 31, 2, 0xC0, pci_read16(0, 31, 2, 0xC0));
	pci_write16(0, 31, 2, 0xC0, 0);
	pci_write16(0, 31, 4, 0xC0, pci_read16(0, 31, 4, 0xC0));
	pci_write16(0, 31, 4, 0xC0, 0);


	/* Open the SMRAM aperture and load our ELF. */
	smram_state_t old_smramc = smram_save_state();

	if (smram_aseg_set_state(SMRAM_ASEG_OPEN) != 0)
	{
		puts("Opening SMRAM failed; cannot load ELF.\n");
	}
	else
	{
		load_elf(mods[0].mod_start, mods[0].mod_end - mods[0].mod_start);
		smram_restore_state(old_smramc);
	}

	outb(0x830, inb(0x830) | 0x41);	/* turn on the SMIs we want */
	
	puts("Waiting for a bit before returning to real mode...");
	for (i=0; i<0x500000; i++)
	{
		if ((i % 0x100000) == 0)
			puts(".");
		inb(0x80);
	}
	puts("\n");

	puts("Now returning to real mode.\n");	
	memcpy(0x4000, _binary_realmode_bin_start, (int)&_binary_realmode_bin_size);
	realmode();	// goodbye!
}
