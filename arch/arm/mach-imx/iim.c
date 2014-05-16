/*
 * iim.c - i.MX IIM fusebox driver
 *
 * Provide an interface for programming and sensing the information that are
 * stored in on-chip fuse elements. This functionality is part of the IC
 * Identification Module (IIM), which is present on some i.MX CPUs.
 *
 * Copyright (c) 2010 Baruch Siach <baruch@tkos.co.il>,
 * 	Orex Computed Radiography
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <common.h>
#include <init.h>
#include <driver.h>
#include <xfuncs.h>
#include <errno.h>
#include <param.h>
#include <fcntl.h>
#include <malloc.h>
#include <of.h>
#include <io.h>

#include <mach/iim.h>

#define DRIVERNAME	"imx_iim"
#define IIM_NUM_BANKS	8

static int iim_write_enable;
static int iim_sense_enable;

struct iim_priv;

struct iim_bank {
	struct cdev cdev;
	void __iomem *bankbase;
	int bank;
	struct iim_priv *iim;
};

struct iim_priv {
	struct cdev cdev;
	struct device_d dev;
	void __iomem *base;
	void __iomem *bankbase;
	struct iim_bank *bank[IIM_NUM_BANKS];
};

static int imx_iim_fuse_sense(struct iim_bank *bank, unsigned int row)
{
	struct iim_priv *iim = bank->iim;
	void __iomem *reg_base = iim->base;
	u8 err, stat;

	if (row > 255) {
		dev_err(&iim->dev, "%s: invalid row index\n", __func__);
		return -EINVAL;
	}

	/* clear status and error registers */
	writeb(3, reg_base + IIM_STATM);
	writeb(0xfe, reg_base + IIM_ERR);

	/* upper and lower address halves */
	writeb((bank->bank << 3) | (row >> 5), reg_base + IIM_UA);
	writeb((row << 3) & 0xf8, reg_base + IIM_LA);

	/* start fuse sensing */
	writeb(0x08, reg_base + IIM_FCTL);

	/* wait for sense done */
	while ((readb(reg_base + IIM_STAT) & 0x80) != 0)
		;

	stat = readb(reg_base + IIM_STAT);
	writeb(stat, reg_base + IIM_STAT);

	err = readb(reg_base + IIM_ERR);
	if (err) {
		dev_err(&iim->dev, "sense error (0x%02x)\n", err);
		return -EIO;
	}

	return readb(reg_base + IIM_SDAT);
}

static ssize_t imx_iim_cdev_read(struct cdev *cdev, void *buf, size_t count,
		loff_t offset, ulong flags)
{
	ulong size, i;
	struct iim_bank *bank = container_of(cdev, struct iim_bank, cdev);

	size = min((loff_t)count, 32 - offset);
	if (iim_sense_enable) {
		for (i = 0; i < size; i++) {
			int row_val;

			row_val = imx_iim_fuse_sense(bank, offset + i);
			if (row_val < 0)
				return row_val;
			((u8 *)buf)[i] = (u8)row_val;
		}
	} else {
		for (i = 0; i < size; i++)
			((u8 *)buf)[i] = ((u8 *)bank->bankbase)[(offset+i)*4];
	}

	return size;
}

static int imx_iim_fuse_blow(struct iim_bank *bank, unsigned int row, u8 value)
{
	struct iim_priv *iim = bank->iim;
	void __iomem *reg_base = iim->base;
	int bit, ret = 0;
	u8 err, stat;

	if (row > 255) {
		dev_err(&iim->dev, "%s: invalid row index\n", __func__);
		return -EINVAL;
	}

	/* clear status and error registers */
	writeb(3, reg_base + IIM_STATM);
	writeb(0xfe, reg_base + IIM_ERR);

	/* unprotect fuse programing */
	writeb(0xaa, reg_base + IIM_PREG_P);

	/* upper half address register */
	writeb((bank->bank << 3) | (row >> 5), reg_base + IIM_UA);

	for (bit = 0; bit < 8; bit++) {
		if (((value >> bit) & 1) == 0)
			continue;

		/* lower half address register */
		writeb(((row << 3) | bit), reg_base + IIM_LA);

		/* start fuse programing */
		writeb(0x71, reg_base + IIM_FCTL);

		/* wait for program done */
		while ((readb(reg_base + IIM_STAT) & 0x80) != 0)
			;

		/* clear program done status */
		stat = readb(reg_base + IIM_STAT);
		writeb(stat, reg_base + IIM_STAT);

		err = readb(reg_base + IIM_ERR);
		if (err) {
			dev_err(&iim->dev, "bank %u, row %u, bit %d program error "
					"(0x%02x)\n", bank->bank, row, bit,
					err);
			ret = -EIO;
			goto out;
		}
	}

out:
	/* protect fuse programing */
	writeb(0, reg_base + IIM_PREG_P);
	return ret;
}

static ssize_t imx_iim_cdev_write(struct cdev *cdev, const void *buf, size_t count,
		loff_t offset, ulong flags)
{
	ulong size, i;
	struct iim_bank *bank = container_of(cdev, struct iim_bank, cdev);

	size = min((loff_t)count, 32 - offset);

	if (IS_ENABLED(CONFIG_IMX_IIM_FUSE_BLOW) && iim_write_enable) {
		for (i = 0; i < size; i++) {
			int ret;

			ret = imx_iim_fuse_blow(bank, offset + i, ((u8 *)buf)[i]);
			if (ret < 0)
				return ret;
		}
	} else {
		for (i = 0; i < size; i++)
			((u8 *)bank->bankbase)[(offset+i)*4] = ((u8 *)buf)[i];
	}

	return size;
}

static struct file_operations imx_iim_ops = {
	.read	= imx_iim_cdev_read,
	.write	= imx_iim_cdev_write,
	.lseek	= dev_lseek_default,
};

static int imx_iim_add_bank(struct iim_priv *iim, int num)
{
	struct iim_bank *bank;
	struct cdev *cdev;

	bank = xzalloc(sizeof (*bank));

	bank->bankbase	= iim->base + 0x800 + 0x400 * num;
	bank->bank	= num;
	bank->iim	= iim;
	cdev = &bank->cdev;
	cdev->ops	= &imx_iim_ops;
	cdev->size	= 32;
	cdev->name	= asprintf(DRIVERNAME "_bank%d", num);
	if (cdev->name == NULL)
		return -ENOMEM;

	iim->bank[num] = bank;

	return devfs_create(cdev);
}

#if IS_ENABLED(CONFIG_OFDEVICE)

/*
 * a single MAC address reference has the form
 * <&phandle iim-bank-no offset>, so three cells
 */
#define MAC_ADDRESS_PROPLEN	(3 * sizeof(__be32))

static void imx_iim_init_dt(struct device_d *dev)
{
	char mac[6];
	const __be32 *prop;
	struct device_node *node = dev->device_node;
	int len, ret;

	if (!node)
		return;

	prop = of_get_property(node, "barebox,provide-mac-address", &len);
	if (!prop)
		return;

	while (len >= MAC_ADDRESS_PROPLEN) {
		struct device_node *rnode;
		uint32_t phandle, bank, offset;

		phandle = be32_to_cpup(prop++);

		rnode = of_find_node_by_phandle(phandle);
		bank = be32_to_cpup(prop++);
		offset = be32_to_cpup(prop++);

		ret = imx_iim_read(bank, offset, mac, 6);
		if (ret == 6) {
			of_eth_register_ethaddr(rnode, mac);
		} else {
			dev_err(dev, "cannot read: %s\n", strerror(-ret));
		}

		len -= MAC_ADDRESS_PROPLEN;
	}
}
#else
static inline void imx_iim_init_dt(struct device_d *dev)
{
}
#endif

static int imx_iim_probe(struct device_d *dev)
{
	struct iim_priv *iim;
	int i, ret;

	iim = xzalloc(sizeof(*iim));

	strcpy(iim->dev.name, "iim");
	iim->dev.parent = dev;
	iim->dev.id = DEVICE_ID_SINGLE;
	ret = register_device(&iim->dev);
	if (ret)
		return ret;

	iim->base = dev_request_mem_region(dev, 0);
	if (!iim->base)
		return -EBUSY;

	for (i = 0; i < IIM_NUM_BANKS; i++) {
		ret = imx_iim_add_bank(iim, i);
		if (ret)
			return ret;
	}

	imx_iim_init_dt(dev, iim);

	if (IS_ENABLED(CONFIG_IMX_IIM_FUSE_BLOW))
		dev_add_param_bool(&iim->dev, "permanent_write_enable",
			NULL, NULL, &iim_write_enable, NULL);

	dev_add_param_bool(&iim->dev, "explicit_sense_enable",
			NULL, NULL, &iim_sense_enable, NULL);

	return 0;
}

static __maybe_unused struct of_device_id imx_iim_dt_ids[] = {
	{
		.compatible = "fsl,imx27-iim",
	}, {
		/* sentinel */
	}
};

static struct driver_d imx_iim_driver = {
	.name	= DRIVERNAME,
	.probe	= imx_iim_probe,
	.of_compatible = DRV_OF_COMPAT(imx_iim_dt_ids),
};

static int imx_iim_init(void)
{
	platform_driver_register(&imx_iim_driver);

	return 0;
}
coredevice_initcall(imx_iim_init);

int imx_iim_read(unsigned int bank, int offset, void *buf, int count)
{
	struct cdev *cdev;
	char *name = asprintf(DRIVERNAME "_bank%d", bank);
	int ret;

	cdev = cdev_open(name, O_RDONLY);
	if (!cdev)
		return -ENODEV;

	ret = cdev_read(cdev, buf, count, offset, 0);

	cdev_close(cdev);
	free(name);

	return ret;
}
