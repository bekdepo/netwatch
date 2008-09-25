#include <io.h>
#include <smi.h>
#include <pci.h>
#include <reg-82801b.h>
#include "vga-overlay.h"
#include <smram.h>

extern int _bss, _bssend;

void __firstrun_start() {
	unsigned char *bp;
	smram_state_t smram;
	
	smram = smram_save_state();
	smram_tseg_set_state(SMRAM_TSEG_OPEN);
	
	for (bp = (void *)&_bss; (void *)bp < (void *)&_bssend; bp++)
		*bp = 0;
	
	dologf("NetWatch running");

	/* Try really hard to shut up USB_LEGKEY. */
	pci_write16(0, 31, 2, 0xC0, pci_read16(0, 31, 2, 0xC0));
	pci_write16(0, 31, 2, 0xC0, 0);
	pci_write16(0, 31, 4, 0xC0, pci_read16(0, 31, 4, 0xC0));
	pci_write16(0, 31, 4, 0xC0, 0);

	/* Turn on the SMIs we want */
	outb(0x830, inb(0x830) | ICH2_SMI_EN_SWSMI_TMR_EN);
	outb(0x848, ICH2_DEVTRAP_EN_KBC_TRP_EN);
	smi_enable();
	
	smram_restore_state(smram);
}

