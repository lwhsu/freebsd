/*-
 * $Id: $
 */

#include "sc.h"
#include "opt_syscons.h"

#if NSC > 0

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>

#include <machine/cons.h>
#include <machine/console.h>
#include <machine/clock.h>

#include <pc98/pc98/pc98.h>
#include <pc98/pc98/pc98_machdep.h>

#include <dev/syscons/syscons.h>

#include <i386/isa/timerreg.h>

#include <isa/isavar.h>

static devclass_t	sc_devclass;

static int	scprobe(device_t dev);
static int	scattach(device_t dev);
static int	scresume(device_t dev);

static device_method_t sc_methods[] = {
	DEVMETHOD(device_probe,         scprobe),
	DEVMETHOD(device_attach,        scattach),
	DEVMETHOD(device_resume,        scresume),
	{ 0, 0 }
};

static driver_t sc_driver = {
	SC_DRIVER_NAME,
	sc_methods,
	1,                          /* XXX */
};

static sc_softc_t main_softc = { 0, 0, 0, -1, NULL, -1, NULL, };

static int
scprobe(device_t dev)
{
	/* No pnp support */
	if (isa_get_vendorid(dev))
		return (ENXIO);

	device_set_desc(dev, "System console");
	return sc_probe_unit(device_get_unit(dev), isa_get_flags(dev));
}

static int
scattach(device_t dev)
{
	return sc_attach_unit(device_get_unit(dev), isa_get_flags(dev));
}

static int
scresume(device_t dev)
{
	return sc_resume_unit(device_get_unit(dev));
}

int
sc_max_unit(void)
{
	return devclass_get_maxunit(sc_devclass);
}

sc_softc_t
*sc_get_softc(int unit, int flags)
{
	sc_softc_t *sc;

	if ((unit < 0) || (unit >= NSC))
		return NULL;
	if (flags & SC_KERNEL_CONSOLE) {
		/* FIXME: clear if it is wired to another unit! */
		main_softc.unit = unit;
		return &main_softc;
	} else {
	        sc = (sc_softc_t *)devclass_get_softc(sc_devclass, unit);
		if (!(sc->flags & SC_INIT_DONE)) {
			sc->unit = unit;
			sc->keyboard = -1;
			sc->adapter = -1;
		}
		return sc;
	}
}

sc_softc_t
*sc_find_softc(struct video_adapter *adp, struct keyboard *kbd)
{
	sc_softc_t *sc;
	int units;
	int i;

	sc = &main_softc;
	if (((adp == NULL) || (adp == sc->adp))
	    && ((kbd == NULL) || (kbd == sc->kbd)))
		return sc;
	units = devclass_get_maxunit(sc_devclass);
	for (i = 0; i < units; ++i) {
	        sc = (sc_softc_t *)devclass_get_softc(sc_devclass, i);
		if (sc == NULL)
			continue;
		if (((adp == NULL) || (adp == sc->adp))
		    && ((kbd == NULL) || (kbd == sc->kbd)))
			return sc;
	}
	return NULL;
}

int
sc_get_cons_priority(int *unit, int *flags)
{
	int disabled;
	int u, f;
	int i;

	*unit = -1;
	for (i = -1; (i = resource_locate(i, SC_DRIVER_NAME)) >= 0;) {
		u = resource_query_unit(i);
		if ((resource_int_value(SC_DRIVER_NAME, u, "disabled",
					&disabled) == 0) && disabled)
			continue;
		if (resource_int_value(SC_DRIVER_NAME, u, "flags", &f) != 0)
			f = 0;
		if (f & SC_KERNEL_CONSOLE) {
			/* the user designates this unit to be the console */
			*unit = u;
			*flags = f;
			break;
		}
		if (*unit < 0) {
			/* ...otherwise remember the first found unit */
			*unit = u;
			*flags = f;
		}
	}
	if ((i < 0) && (*unit < 0))
		return CN_DEAD;
	return CN_INTERNAL;
}

void
sc_get_bios_values(bios_values_t *values)
{
	values->cursor_start = 0;
	values->cursor_end = 16;
	values->shift_state = 0;
	if (pc98_machine_type & M_8M)
		values->bell_pitch = BELL_PITCH_8M;
	else
		values->bell_pitch = BELL_PITCH_5M;
}

int
sc_tone(int herz)
{
	int pitch;

	if (herz) {
		/* enable counter 1 */
		outb(0x35, inb(0x35) & 0xf7);
		/* set command for counter 1, 2 byte write */
		if (acquire_timer1(TIMER_16BIT | TIMER_SQWAVE))
			return EBUSY;
		/* set pitch */
		pitch = timer_freq/herz;
		outb(TIMER_CNTR1, pitch);
		outb(TIMER_CNTR1, pitch >> 8);
	} else {
		/* disable counter 1 */
		outb(0x35, inb(0x35) | 0x08);
		release_timer1();
	}
	return 0;
}

DRIVER_MODULE(sc, isa, sc_driver, sc_devclass, 0, 0);

#endif /* NSC > 0 */
