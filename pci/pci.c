#include <pci.h>
#include <output.h>

int pci_probe(pci_probe_fn_t probe)
{
	int devsfound = 0;
	pci_dev_t pdev;
	unsigned int bus;
	unsigned int dev;
	unsigned int fn;
	
	for (bus = 0; bus < 0x100; bus++)
		for (dev = 0; dev < 0x20; dev++)
			if (pci_read32(bus, dev, 0, 0) != 0xFFFFFFFF)
				for (fn = 0; fn < 8; fn++)
				{
					int bar;
					
					if (pci_read32(bus, dev, fn, 0) == 0xFFFFFFFF)
						continue;
					
					if ((fn != 0) && !(pci_read8(bus, dev, 0, 0x0E) & 0x80))
						continue;
					
					pdev.bus = bus;
					pdev.dev = dev;
					pdev.fn = fn;
					pdev.vid = pci_read16(bus, dev, fn, 0);
					pdev.did = pci_read16(bus, dev, fn, 2);
					
					for (bar = 0; bar < 6; bar++)
					{
						unsigned long bardat = pci_read32(bus, dev, fn, 0x10 + bar*4);
						if (bardat == 0)
						{
							pdev.bars[bar].type = PCI_BAR_NONE;
							continue;
						}
						if (bardat & 1)
						{
							pdev.bars[bar].type = PCI_BAR_IO;
							pdev.bars[bar].addr = bardat & ~0x3;
						} else {
							pdev.bars[bar].prefetchable = (bardat >> 3) & 1;
							switch ((bardat >> 1) & 0x3)
							{
							case 0:
								pdev.bars[bar].type = PCI_BAR_MEMORY32;
								pdev.bars[bar].addr = bardat & ~0xF;
								break;
							case 2:
								pdev.bars[bar].type = PCI_BAR_MEMORY64;
								bar++;
								pdev.bars[bar].type = PCI_BAR_NONE;
								break;
							default:
								pdev.bars[bar].type = PCI_BAR_NONE;
								continue;
							}
						}
					}
					
					devsfound += probe(&pdev);
				}
	
	return devsfound;
}

static int _enumfn(pci_dev_t *pdev)
{
	int bar;
	
	outputf("Found device: %02x:%02x.%1x: %04X:%04X",
		pdev->bus, pdev->dev, pdev->fn,
		pdev->vid, pdev->did);
	for (bar = 0; bar < 6; bar++)
	{
		switch (pdev->bars[bar].type)
		{
		case PCI_BAR_IO:
			outputf("  BAR %d: I/O, Addr %04x", bar, pdev->bars[bar].addr);
			break;
		case PCI_BAR_MEMORY32:
			outputf("  BAR %d: Mem32, Addr %04x", bar, pdev->bars[bar].addr);
			break;
		case PCI_BAR_MEMORY64:
			outputf("  BAR %d: Mem64, Addr %04x", bar, pdev->bars[bar].addr);
			break;
		default:
			break;
		}
	}
	return 0;
}

void pci_bus_enum()
{
	pci_probe(_enumfn);
}

