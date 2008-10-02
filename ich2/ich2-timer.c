#include <pci.h>
#include <io.h>
#include <reg-82801b.h>

static uint16_t _get_PMBASE()
{
	static long pmbase = -1;
	
	if (pmbase == -1)	/* Memoize it so that we don't have to hit PCI so often. */
		pmbase = pci_read32(ICH2_LPC_BUS, ICH2_LPC_DEV, ICH2_LPC_FN, ICH2_LPC_PCI_PMBASE) & ICH2_PMBASE_MASK;
	
	return pmbase;
}

static unsigned long _curtmr()
{
	return inl(_get_PMBASE + ICH2_PMBASE_PM1_TMR) & 0xFFFFFF;
}

/* This is kind of a heuristic, since we can't get interrupts. */
static unsigned long starttmr = 0, endtmr = 0;

void oneshot_start_ms(unsigned long milliseconds)
{
	starttmr = _curtmr();
	endtmr = (starttmr + milliseconds*(ICH2_PM1_TMR_FREQ/1000)) % 0xFFFFFF;
}

int oneshot_running(void)
{
	if (endtmr == 0 && starttmr == 0)
		return 0;
	if ((endtmr < starttmr) && ((_curtmr() > starttmr) || (_curtmr() < endtmr)))
		return 1;
	else if ((_curtmr() < endtmr) && (_curtmr() > starttmr))
		return 1;
	endtmr = starttmr = 0;
	return 0;
}
