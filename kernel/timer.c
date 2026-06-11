/* timer.c — program the PIT and count IRQ0 ticks. */

#include "timer.h"
#include "irq.h"
#include "io.h"
#include "nic.h"

#define PIT_BASE_HZ   1193182      /* the PIT's fixed input frequency        */
#define PIT_CH0_DATA  0x40
#define PIT_CMD       0x43
#define PIT_MODE2     0x34         /* ch0, lo/hi byte, rate generator, binary.
                                    * Mode 2 (not square-wave mode 3) so the
                                    * counter counts down exactly once per tick
                                    * — unambiguous for sub-tick timing in
                                    * timer_us(). */
#define PIT_LATCH_CH0 0x00         /* counter-latch command for channel 0     */
#define PIC1_CMD      0x20
#define PIC_READ_IRR  0x0A         /* OCW3: select the IRR for the next read  */

#define PIT_DIVISOR   (PIT_BASE_HZ / TIMER_HZ)
#define US_PER_TICK   (1000000u / TIMER_HZ)

static volatile uint32_t ticks;
static timer_tick_fn      tick_cb;

static void timer_handler(struct registers *r)
{
	(void)r;
	ticks++;
	nic_poll();                    /* IRQ-independent NIC RX drain (VMware etc.) */
	if (tick_cb)
		tick_cb();             /* e.g. the scheduler preempts here        */
}

void timer_on_tick(timer_tick_fn cb)
{
	tick_cb = cb;
}

void timer_init(void)
{
	uint32_t divisor = PIT_DIVISOR;

	outb(PIT_CMD, PIT_MODE2);
	outb(PIT_CH0_DATA, divisor & 0xFF);
	outb(PIT_CH0_DATA, (divisor >> 8) & 0xFF);

	irq_register(0, timer_handler);
	irq_unmask(0);
}

uint32_t timer_ticks(void)
{
	return ticks;
}

/* Microseconds since boot, at PIT resolution (~0.8 us). Combines the tick
 * count with the live channel-0 down-counter. The tricky case is a counter
 * wrap whose IRQ0 hasn't been serviced yet (e.g. we're called from another
 * interrupt handler, or with IF clear): the latched count has already
 * reloaded near PIT_DIVISOR while `ticks` is still one behind. We detect the
 * pending IRQ via the PIC's IRR and, when the count sits in the first half of
 * the period (so the reload truly preceded our latch), credit the extra tick.
 * Safe to call from interrupt context. */
uint32_t timer_us(void)
{
	uint32_t fl;
	__asm__ volatile ("pushf\n\tpop %0\n\tcli" : "=r"(fl) :: "memory");

	outb(PIT_CMD, PIT_LATCH_CH0);
	uint32_t count = inb(PIT_CH0_DATA);
	count |= (uint32_t)inb(PIT_CH0_DATA) << 8;

	outb(PIC1_CMD, PIC_READ_IRR);
	uint8_t irr = inb(PIC1_CMD);
	uint32_t t = ticks;

	if (fl & 0x200)
		__asm__ volatile ("sti" ::: "memory");

	if ((irr & 0x01) && count > PIT_DIVISOR / 2)
		t++;                            /* wrap already happened, tick pending */

	uint32_t frac = PIT_DIVISOR - count;    /* input clocks into this tick     */
	uint64_t us = (uint64_t)t * US_PER_TICK
	            + (uint64_t)frac * 1000000u / PIT_BASE_HZ;
	return (uint32_t)us;
}

uint32_t timer_seconds(void)
{
	return ticks / TIMER_HZ;
}

void timer_sleep(uint32_t ms)
{
	/* Round up so a short sleep never returns early. */
	uint32_t wait = (ms * TIMER_HZ + 999) / 1000;
	uint32_t target = ticks + wait;
	while (ticks < target)
		__asm__ volatile ("hlt");  /* woken by the timer IRQ                  */
}
