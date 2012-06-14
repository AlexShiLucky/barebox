/*
 * ocotp.c - barebox driver for the On-Chip One Time Programmable for MXS
 *
 * Copyright (C) 2012 by Wolfram Sang, Pengutronix e.K.
 * based on the kernel driver which is
 * Copyright 2010 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <common.h>
#include <init.h>
#include <driver.h>
#include <xfuncs.h>
#include <errno.h>
#include <param.h>
#include <fcntl.h>
#include <malloc.h>
#include <io.h>
#include <clock.h>

#include <mach/generic.h>
#include <mach/ocotp.h>
#include <mach/imx-regs.h>

#define DRIVERNAME "ocotp"

#define OCOTP_CTRL			0x0
#  define OCOTP_CTRL_BUSY		(1 << 8)
#  define OCOTP_CTRL_ERROR		(1 << 9)
#  define OCOTP_CTRL_RD_BANK_OPEN	(1 << 12)

#define OCOTP_WORD_OFFSET		0x20

struct ocotp_priv {
	struct cdev cdev;
	void __iomem *base;
};

static int mxs_ocotp_wait_busy(struct ocotp_priv *priv)
{
	uint64_t start = get_time_ns();

	/* check both BUSY and ERROR cleared */
	while (readl(priv->base + OCOTP_CTRL) & (OCOTP_CTRL_BUSY | OCOTP_CTRL_ERROR))
		if (is_timeout(start, MSECOND))
			return -ETIMEDOUT;

	return 0;
}

static ssize_t mxs_ocotp_cdev_read(struct cdev *cdev, void *buf, size_t count,
		ulong offset, ulong flags)
{
	struct ocotp_priv *priv = cdev->priv;
	void __iomem *base = priv->base;
	size_t size = min((ulong)count, cdev->size - offset);
	int i;

	/*
	 * clk_enable(hbus_clk) for ocotp can be skipped
	 * as it must be on when system is running.
	 */

	/* try to clear ERROR bit */
	writel(OCOTP_CTRL_ERROR, base + OCOTP_CTRL + BIT_CLR);

	if (mxs_ocotp_wait_busy(priv))
		return -ETIMEDOUT;

	/* open OCOTP banks for read */
	writel(OCOTP_CTRL_RD_BANK_OPEN, base + OCOTP_CTRL + BIT_SET);

	/* approximately wait 32 hclk cycles */
	udelay(1);

	/* poll BUSY bit becoming cleared */
	if (mxs_ocotp_wait_busy(priv))
		return -ETIMEDOUT;

	for (i = 0; i < size; i++)
		/* When reading bytewise, we need to hop over the SET/CLR/TGL regs */
		((u8 *)buf)[i] = readb(base + OCOTP_WORD_OFFSET +
				(((i + offset) & 0xfc) << 2) + ((i + offset) & 3));

	/* close banks for power saving */
	writel(OCOTP_CTRL_RD_BANK_OPEN, base + OCOTP_CTRL + BIT_CLR);

	return size;
}

static struct file_operations mxs_ocotp_ops = {
	.read	= mxs_ocotp_cdev_read,
	.lseek	= dev_lseek_default,
};

static int mxs_ocotp_probe(struct device_d *dev)
{
	int err;
	struct ocotp_priv *priv = xzalloc(sizeof (*priv));

	priv->base = dev_request_mem_region(dev, 0);
	priv->cdev.dev = dev;
	priv->cdev.ops = &mxs_ocotp_ops;
	priv->cdev.priv = priv;
	priv->cdev.size = cpu_is_mx23() ? 128 : 160;
	priv->cdev.name = DRIVERNAME;

	err = devfs_create(&priv->cdev);
	if (err < 0)
		return err;

	return 0;
}

static struct driver_d mxs_ocotp_driver = {
	.name	= DRIVERNAME,
	.probe	= mxs_ocotp_probe,
};

static int mxs_ocotp_init(void)
{
	register_driver(&mxs_ocotp_driver);

	return 0;
}
coredevice_initcall(mxs_ocotp_init);

int mxs_ocotp_read(void *buf, int count, int offset)
{
	struct cdev *cdev;
	int ret;

	cdev = cdev_open(DRIVERNAME, O_RDONLY);
	if (!cdev)
		return -ENODEV;

	ret = cdev_read(cdev, buf, count, offset, 0);

	cdev_close(cdev);

	return ret;
}
