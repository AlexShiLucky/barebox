/*
 * Copyright (C) 2012 Antony Pavlov <antonynpavlov@gmail.com>
 *
 * This file is part of barebox.
 * See file CREDITS for list of people who contributed to this project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file
 * @brief Clocksource based on JZ4740 timer
 */

#include <init.h>
#include <clock.h>
#include <io.h>
#include <mach/jz4740_regs.h>

#define JZ_TIMER_CLOCK 40000

static int hz;

static uint64_t jz4740_cs_read(void)
{
	//return (uint64_t)__raw_readl((void *)TCU_OSTCNT);
	hz = hz + 10;
	return hz;
}

static struct clocksource jz4740_cs = {
	.read	= jz4740_cs_read,
	.mask   = CLOCKSOURCE_MASK(32),
	.shift  = 10,
};

static int clocksource_init(void)
{
	jz4740_cs.mult = clocksource_hz2mult(JZ_TIMER_CLOCK, jz4740_cs.shift);
	init_clock(&jz4740_cs);

	hz = 0;
#if 0
	__raw_writel(TCU_OSTCSR_PRESCALE1 | TCU_OSTCSR_EXT_EN,
		(void *)TCU_OSTCSR);
	__raw_writel(0, (void *)TCU_OSTCNT);
	__raw_writel(0xffffffff, (void *)TCU_OSTDR);

	/* enable timer clock */
	__raw_writel(TCU_TSCR_OSTSC, (void *)TCU_TSCR);
	/* start counting up */
	__raw_writel(TCU_TESR_OSTST, (void *)TCU_TESR);
#endif

	return 0;
}
core_initcall(clocksource_init);
