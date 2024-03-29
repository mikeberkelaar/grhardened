#ifndef _I8042_X86IA64IO_H
#define _I8042_X86IA64IO_H

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

/*
 * Names.
 */

#define I8042_KBD_PHYS_DESC "isa0060/serio0"
#define I8042_AUX_PHYS_DESC "isa0060/serio1"
#define I8042_MUX_PHYS_DESC "isa0060/serio%d"

/*
 * IRQs.
 */

#if defined(__ia64__)
# define I8042_MAP_IRQ(x)	isa_irq_to_vector((x))
#else
# define I8042_MAP_IRQ(x)	(x)
#endif

#define I8042_KBD_IRQ	i8042_kbd_irq
#define I8042_AUX_IRQ	i8042_aux_irq

static int i8042_kbd_irq;
static int i8042_aux_irq;

/*
 * Register numbers.
 */

#define I8042_COMMAND_REG	i8042_command_reg
#define I8042_STATUS_REG	i8042_command_reg
#define I8042_DATA_REG		i8042_data_reg

static int i8042_command_reg = 0x64;
static int i8042_data_reg = 0x60;


static inline int i8042_read_data(void)
{
	return inb(I8042_DATA_REG);
}

static inline int i8042_read_status(void)
{
	return inb(I8042_STATUS_REG);
}

static inline void i8042_write_data(int val)
{
	outb(val, I8042_DATA_REG);
}

static inline void i8042_write_command(int val)
{
	outb(val, I8042_COMMAND_REG);
}

#ifdef CONFIG_X86

#include <linux/dmi.h>

static struct dmi_system_id __initdata i8042_dmi_noloop_table[] = {
	{
		/* AUX LOOP command does not raise AUX IRQ */
		.ident = "Arima-Rioworks HDAMB",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "RIOWORKS"),
			DMI_MATCH(DMI_BOARD_NAME, "HDAMB"),
			DMI_MATCH(DMI_BOARD_VERSION, "Rev E"),
		},
	},
	{
		/* AUX LOOP command does not raise AUX IRQ */
		.ident = "ASUS P65UP5",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "ASUSTeK Computer INC."),
			DMI_MATCH(DMI_BOARD_NAME, "P/I-P65UP5"),
			DMI_MATCH(DMI_BOARD_VERSION, "REV 2.X"),
		},
	},
	{
		.ident = "Compaq Proliant 8500",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Compaq"),
			DMI_MATCH(DMI_PRODUCT_NAME , "ProLiant"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "8500"),
		},
	},
	{
		.ident = "Compaq Proliant DL760",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Compaq"),
			DMI_MATCH(DMI_PRODUCT_NAME , "ProLiant"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "DL760"),
		},
	},
	{
		.ident = "OQO Model 01",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "OQO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "ZEPTO"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "00"),
		},
	},
	{
		/* AUX LOOP does not work properly */
		.ident = "ULI EV4873",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ULI"),
			DMI_MATCH(DMI_PRODUCT_NAME, "EV4873"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "5a"),
		},
	},
	{
		.ident = "Microsoft Virtual Machine",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Microsoft Corporation"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Virtual Machine"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "VS2005R2"),
		},
	},
	{
		.ident = "Medion MAM 2070",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Notebook"),
			DMI_MATCH(DMI_PRODUCT_NAME, "MAM 2070"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "5a"),
		},
	},
	{
		.ident = "Blue FB5601",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "blue"),
			DMI_MATCH(DMI_PRODUCT_NAME, "FB5601"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "M606"),
		},
	},
	{ NULL, NULL, {DMI_MATCH(DMI_NONE, {0})}, NULL }
};

/*
 * Some Fujitsu notebooks are having trouble with touchpads if
 * active multiplexing mode is activated. Luckily they don't have
 * external PS/2 ports so we can safely disable it.
 * ... apparently some Toshibas don't like MUX mode either and
 * die horrible death on reboot.
 */
static struct dmi_system_id __initdata i8042_dmi_nomux_table[] = {
	{
		.ident = "Fujitsu Lifebook P7010/P7010D",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU"),
			DMI_MATCH(DMI_PRODUCT_NAME, "P7010"),
		},
	},
	{
		.ident = "Fujitsu Lifebook P7010",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU SIEMENS"),
			DMI_MATCH(DMI_PRODUCT_NAME, "0000000000"),
		},
	},
	{
		.ident = "Fujitsu Lifebook P5020D",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU"),
			DMI_MATCH(DMI_PRODUCT_NAME, "LifeBook P Series"),
		},
	},
	{
		.ident = "Fujitsu Lifebook S2000",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU"),
			DMI_MATCH(DMI_PRODUCT_NAME, "LifeBook S Series"),
		},
	},
	{
		.ident = "Fujitsu Lifebook S6230",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU"),
			DMI_MATCH(DMI_PRODUCT_NAME, "LifeBook S6230"),
		},
	},
	{
		.ident = "Fujitsu T70H",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU"),
			DMI_MATCH(DMI_PRODUCT_NAME, "FMVLT70H"),
		},
	},
	{
		.ident = "Fujitsu-Siemens Lifebook T3010",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU SIEMENS"),
			DMI_MATCH(DMI_PRODUCT_NAME, "LIFEBOOK T3010"),
		},
	},
	{
		.ident = "Fujitsu-Siemens Lifebook E4010",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU SIEMENS"),
			DMI_MATCH(DMI_PRODUCT_NAME, "LIFEBOOK E4010"),
		},
	},
	{
		.ident = "Fujitsu-Siemens Amilo Pro 2010",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU SIEMENS"),
			DMI_MATCH(DMI_PRODUCT_NAME, "AMILO Pro V2010"),
		},
	},
	{
		.ident = "Fujitsu-Siemens Amilo Pro 2030",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "FUJITSU SIEMENS"),
			DMI_MATCH(DMI_PRODUCT_NAME, "AMILO PRO V2030"),
		},
	},
	{
		/*
		 * No data is coming from the touchscreen unless KBC
		 * is in legacy mode.
		 */
		.ident = "Panasonic CF-29",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Matsushita"),
			DMI_MATCH(DMI_PRODUCT_NAME, "CF-29"),
		},
	},
	{
		/*
		 * Errors on MUX ports are reported without raising AUXDATA
		 * causing "spurious NAK" messages.
		 */
		.ident = "HP Pavilion DV4017EA",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Pavilion dv4000 (EA032EA#ABF)"),
		},
	},
	{
		/*
		 * Like DV4017EA does not raise AUXERR for errors on MUX ports.
		 */
		.ident = "HP Pavilion ZT1000",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
			DMI_MATCH(DMI_PRODUCT_NAME, "HP Pavilion Notebook PC"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "HP Pavilion Notebook ZT1000"),
		},
	},
	{
		/*
		 * Like DV4017EA does not raise AUXERR for errors on MUX ports.
		 */
		.ident = "HP Pavilion DV4270ca",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Hewlett-Packard"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Pavilion dv4000 (EH476UA#ABL)"),
		},
	},
	{
		.ident = "Toshiba P10",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TOSHIBA"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Satellite P10"),
		},
	},
	{
		.ident = "Toshiba Equium A110",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "TOSHIBA"),
			DMI_MATCH(DMI_PRODUCT_NAME, "EQUIUM A110"),
		},
	},
	{
		.ident = "Alienware Sentia",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ALIENWARE"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Sentia"),
		},
	},
	{
		.ident = "Sharp Actius MM20",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SHARP"),
			DMI_MATCH(DMI_PRODUCT_NAME, "PC-MM20 Series"),
		},
	},
	{
		.ident = "Sony Vaio FS-115b",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Sony Corporation"),
			DMI_MATCH(DMI_PRODUCT_NAME, "VGN-FS115B"),
		},
	},
	{
		.ident = "Amoi M636/A737",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Amoi Electronics CO.,LTD."),
			DMI_MATCH(DMI_PRODUCT_NAME, "M636/A737 platform"),
		},
	},
	{
		.ident = "Lenovo 3000 n100",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_NAME, "076804U"),
		},
	},
	{
		.ident = "Acer Aspire 1360",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 1360"),
		},
	},
	{
		.ident = "Gericom Bellagio",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Gericom"),
			DMI_MATCH(DMI_PRODUCT_NAME, "N34AS6"),
		},
	},
	{
		.ident = "IBM 2656",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "IBM"),
			DMI_MATCH(DMI_PRODUCT_NAME, "2656"),
		},
	},
	{
		.ident = "Dell XPS M1530",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "XPS M1530"),
		},
	},
	{
		.ident = "Compal HEL80I",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "COMPAL"),
			DMI_MATCH(DMI_PRODUCT_NAME, "HEL80I"),
		},
	},
	{ NULL, NULL, {DMI_MATCH(DMI_NONE, {0})}, NULL }
};

#ifdef CONFIG_PNP
static struct dmi_system_id __initdata i8042_dmi_nopnp_table[] = {
	{
		.ident = "Intel MBO Desktop D845PESV",
		.matches = {
			DMI_MATCH(DMI_BOARD_NAME, "D845PESV"),
			DMI_MATCH(DMI_BOARD_VENDOR, "Intel Corporation"),
		},
	},
	{ NULL, NULL, {DMI_MATCH(DMI_NONE, {0})}, NULL }
};
#endif

/*
 * Some Wistron based laptops need us to explicitly enable the 'Dritek
 * keyboard extension' to make their extra keys start generating scancodes.
 * Originally, this was just confined to older laptops, but a few Acer laptops
 * have turned up in 2007 that also need this again.
 */
static struct dmi_system_id __initdata i8042_dmi_dritek_table[] = {
	{
		.ident = "Acer Aspire 5630",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5630"),
		},
	},
	{
		.ident = "Acer Aspire 5650",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5650"),
		},
	},
	{
		.ident = "Acer Aspire 5680",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5680"),
		},
	},
	{
		.ident = "Acer Aspire 5720",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 5720"),
		},
	},
	{
		.ident = "Acer Aspire 9110",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire 9110"),
		},
	},
	{
		.ident = "Acer TravelMate 660",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate 660"),
		},
	},
	{
		.ident = "Acer TravelMate 2490",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate 2490"),
		},
	},
	{
		.ident = "Acer TravelMate 4280",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate 4280"),
		},
	},
	{ NULL, NULL, {DMI_MATCH(DMI_NONE, {0})}, NULL }
};

#endif /* CONFIG_X86 */

#ifdef CONFIG_PNP
#include <linux/pnp.h>

static int i8042_pnp_kbd_registered;
static unsigned int i8042_pnp_kbd_devices;
static int i8042_pnp_aux_registered;
static unsigned int i8042_pnp_aux_devices;

static int i8042_pnp_command_reg;
static int i8042_pnp_data_reg;
static int i8042_pnp_kbd_irq;
static int i8042_pnp_aux_irq;

static char i8042_pnp_kbd_name[32];
static char i8042_pnp_aux_name[32];

static int i8042_pnp_kbd_probe(struct pnp_dev *dev, const struct pnp_device_id *did)
{
	if (pnp_port_valid(dev, 0) && pnp_port_len(dev, 0) == 1)
		i8042_pnp_data_reg = pnp_port_start(dev,0);

	if (pnp_port_valid(dev, 1) && pnp_port_len(dev, 1) == 1)
		i8042_pnp_command_reg = pnp_port_start(dev, 1);

	if (pnp_irq_valid(dev,0))
		i8042_pnp_kbd_irq = pnp_irq(dev, 0);

	strlcpy(i8042_pnp_kbd_name, did->id, sizeof(i8042_pnp_kbd_name));
	if (strlen(pnp_dev_name(dev))) {
		strlcat(i8042_pnp_kbd_name, ":", sizeof(i8042_pnp_kbd_name));
		strlcat(i8042_pnp_kbd_name, pnp_dev_name(dev), sizeof(i8042_pnp_kbd_name));
	}

	i8042_pnp_kbd_devices++;
	return 0;
}

static int i8042_pnp_aux_probe(struct pnp_dev *dev, const struct pnp_device_id *did)
{
	if (pnp_port_valid(dev, 0) && pnp_port_len(dev, 0) == 1)
		i8042_pnp_data_reg = pnp_port_start(dev,0);

	if (pnp_port_valid(dev, 1) && pnp_port_len(dev, 1) == 1)
		i8042_pnp_command_reg = pnp_port_start(dev, 1);

	if (pnp_irq_valid(dev, 0))
		i8042_pnp_aux_irq = pnp_irq(dev, 0);

	strlcpy(i8042_pnp_aux_name, did->id, sizeof(i8042_pnp_aux_name));
	if (strlen(pnp_dev_name(dev))) {
		strlcat(i8042_pnp_aux_name, ":", sizeof(i8042_pnp_aux_name));
		strlcat(i8042_pnp_aux_name, pnp_dev_name(dev), sizeof(i8042_pnp_aux_name));
	}

	i8042_pnp_aux_devices++;
	return 0;
}

static struct pnp_device_id pnp_kbd_devids[] = {
	{ .id = "PNP0303", .driver_data = 0 },
	{ .id = "PNP030b", .driver_data = 0 },
	{ .id = "", },
};

static struct pnp_driver i8042_pnp_kbd_driver = {
	.name           = "i8042 kbd",
	.id_table       = pnp_kbd_devids,
	.probe          = i8042_pnp_kbd_probe,
};

static struct pnp_device_id pnp_aux_devids[] = {
	{ .id = "FJC6000", .driver_data = 0 },
	{ .id = "FJC6001", .driver_data = 0 },
	{ .id = "PNP0f03", .driver_data = 0 },
	{ .id = "PNP0f0b", .driver_data = 0 },
	{ .id = "PNP0f0e", .driver_data = 0 },
	{ .id = "PNP0f12", .driver_data = 0 },
	{ .id = "PNP0f13", .driver_data = 0 },
	{ .id = "PNP0f19", .driver_data = 0 },
	{ .id = "PNP0f1c", .driver_data = 0 },
	{ .id = "SYN0801", .driver_data = 0 },
	{ .id = "", },
};

static struct pnp_driver i8042_pnp_aux_driver = {
	.name           = "i8042 aux",
	.id_table       = pnp_aux_devids,
	.probe          = i8042_pnp_aux_probe,
};

static void i8042_pnp_exit(void)
{
	if (i8042_pnp_kbd_registered) {
		i8042_pnp_kbd_registered = 0;
		pnp_unregister_driver(&i8042_pnp_kbd_driver);
	}

	if (i8042_pnp_aux_registered) {
		i8042_pnp_aux_registered = 0;
		pnp_unregister_driver(&i8042_pnp_aux_driver);
	}
}

static int __init i8042_pnp_init(void)
{
	char kbd_irq_str[4] = { 0 }, aux_irq_str[4] = { 0 };
	int pnp_data_busted = 0;
	int err;

#ifdef CONFIG_X86
	if (dmi_check_system(i8042_dmi_nopnp_table))
		i8042_nopnp = 1;
#endif

	if (i8042_nopnp) {
		printk(KERN_INFO "i8042: PNP detection disabled\n");
		return 0;
	}

	err = pnp_register_driver(&i8042_pnp_kbd_driver);
	if (!err)
		i8042_pnp_kbd_registered = 1;

	err = pnp_register_driver(&i8042_pnp_aux_driver);
	if (!err)
		i8042_pnp_aux_registered = 1;

	if (!i8042_pnp_kbd_devices && !i8042_pnp_aux_devices) {
		i8042_pnp_exit();
#if defined(__ia64__)
		return -ENODEV;
#else
		printk(KERN_INFO "PNP: No PS/2 controller found. Probing ports directly.\n");
		return 0;
#endif
	}

	if (i8042_pnp_kbd_devices)
		snprintf(kbd_irq_str, sizeof(kbd_irq_str),
			"%d", i8042_pnp_kbd_irq);
	if (i8042_pnp_aux_devices)
		snprintf(aux_irq_str, sizeof(aux_irq_str),
			"%d", i8042_pnp_aux_irq);

	printk(KERN_INFO "PNP: PS/2 Controller [%s%s%s] at %#x,%#x irq %s%s%s\n",
		i8042_pnp_kbd_name, (i8042_pnp_kbd_devices && i8042_pnp_aux_devices) ? "," : "",
		i8042_pnp_aux_name,
		i8042_pnp_data_reg, i8042_pnp_command_reg,
		kbd_irq_str, (i8042_pnp_kbd_devices && i8042_pnp_aux_devices) ? "," : "",
		aux_irq_str);

#if defined(__ia64__)
	if (!i8042_pnp_kbd_devices)
		i8042_nokbd = 1;
	if (!i8042_pnp_aux_devices)
		i8042_noaux = 1;
#endif

	if (((i8042_pnp_data_reg & ~0xf) == (i8042_data_reg & ~0xf) &&
	      i8042_pnp_data_reg != i8042_data_reg) ||
	    !i8042_pnp_data_reg) {
		printk(KERN_WARNING
			"PNP: PS/2 controller has invalid data port %#x; "
			"using default %#x\n",
			i8042_pnp_data_reg, i8042_data_reg);
		i8042_pnp_data_reg = i8042_data_reg;
		pnp_data_busted = 1;
	}

	if (((i8042_pnp_command_reg & ~0xf) == (i8042_command_reg & ~0xf) &&
	      i8042_pnp_command_reg != i8042_command_reg) ||
	    !i8042_pnp_command_reg) {
		printk(KERN_WARNING
			"PNP: PS/2 controller has invalid command port %#x; "
			"using default %#x\n",
			i8042_pnp_command_reg, i8042_command_reg);
		i8042_pnp_command_reg = i8042_command_reg;
		pnp_data_busted = 1;
	}

	if (!i8042_nokbd && !i8042_pnp_kbd_irq) {
		printk(KERN_WARNING
			"PNP: PS/2 controller doesn't have KBD irq; "
			"using default %d\n", i8042_kbd_irq);
		i8042_pnp_kbd_irq = i8042_kbd_irq;
		pnp_data_busted = 1;
	}

	if (!i8042_noaux && !i8042_pnp_aux_irq) {
		if (!pnp_data_busted && i8042_pnp_kbd_irq) {
			printk(KERN_WARNING
				"PNP: PS/2 appears to have AUX port disabled, "
				"if this is incorrect please boot with "
				"i8042.nopnp\n");
			i8042_noaux = 1;
		} else {
			printk(KERN_WARNING
				"PNP: PS/2 controller doesn't have AUX irq; "
				"using default %d\n", i8042_aux_irq);
			i8042_pnp_aux_irq = i8042_aux_irq;
		}
	}

	i8042_data_reg = i8042_pnp_data_reg;
	i8042_command_reg = i8042_pnp_command_reg;
	i8042_kbd_irq = i8042_pnp_kbd_irq;
	i8042_aux_irq = i8042_pnp_aux_irq;

	return 0;
}

#else
static inline int i8042_pnp_init(void) { return 0; }
static inline void i8042_pnp_exit(void) { }
#endif

static int __init i8042_platform_init(void)
{
	int retval;

/*
 * On ix86 platforms touching the i8042 data register region can do really
 * bad things. Because of this the region is always reserved on ix86 boxes.
 *
 *	if (!request_region(I8042_DATA_REG, 16, "i8042"))
 *		return -EBUSY;
 */

	i8042_kbd_irq = I8042_MAP_IRQ(1);
	i8042_aux_irq = I8042_MAP_IRQ(12);

	retval = i8042_pnp_init();
	if (retval)
		return retval;

#if defined(__ia64__)
        i8042_reset = 1;
#endif

#ifdef CONFIG_X86
	if (dmi_check_system(i8042_dmi_noloop_table))
		i8042_noloop = 1;

	if (dmi_check_system(i8042_dmi_nomux_table))
		i8042_nomux = 1;

	if (dmi_check_system(i8042_dmi_dritek_table))
		i8042_dritek = 1;
#endif /* CONFIG_X86 */

	return retval;
}

static inline void i8042_platform_exit(void)
{
	i8042_pnp_exit();
}

#endif /* _I8042_X86IA64IO_H */
