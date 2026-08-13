// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * nuc_ec_hwmon - hwmon driver for the Intel NUC embedded controller
 *
 * Intel NUCs ship no fan sensor to Linux: there is no Super I/O at 0x2E/0x4E
 * (on the NUC11 the window is relocated to 0x164E, where no in-tree driver
 * probes), and the ACPI PNP0C0B fans are ACPI 1.0 on/off devices with no _FST.
 * The data is in the ACPI EC's RAM, which the kernel's EC driver already
 * reaches; this driver reads it and publishes hwmon channels.
 *
 * ACCESS METHOD
 *
 * ec_read(), i.e. the standard ACPI EC protocol over the PNP0C09 device the
 * kernel already owns. No ioremap, no /dev/mem, no physical address, and the
 * ACPI EC driver serialises against AML's own EC traffic for us.
 *
 * A memory-mapped alias of the same bytes exists (the DSDT declares it as
 * OperationRegion FNEC at 0xFE410400 and exposes it as the _CRS of ACPI device
 * INTC1036, which is what this driver binds to). Reading that directly was the
 * first approach here and it worked, but the EC path is strictly better: same
 * data, nothing to go stale, proper locking. The binding is kept because
 * INTC1036 is the natural attach point - Intel's own Windows driver binds to
 * ACPI\INTC1036 too - even though its resource is now unused.
 *
 * FAN BLOCK LAYOUT
 *
 * The EC exposes six 3-byte fan records:
 *
 *	[0] fan type (0 = not connected)
 *	[1] speed, high byte
 *	[2] speed, low byte      -> RPM = high * 256 + low
 *
 * Intel does not put those 18 bytes in the same place on every board. Its NUC
 * Software Studio Service carries an ECVersion enumeration - ECNUC21, ECNUC22,
 * ECNUC23, ECBC, ECElement - and its kernel driver assembles the block
 * differently per "EC section":
 *
 *	section 21:  9 bytes at 0x1A, then 9 bytes at 0x37
 *	section 22:  18 bytes at 0x1C
 *	section BC:  the section-21 read with the first byte dropped and a
 *	             zero appended, i.e. 8 bytes at 0x1B, 9 at 0x37, then 0x00
 *
 * ECNUC21 uses section 21, ECNUC22 and ECNUC23 use section 22, ECBC uses BC,
 * and ECElement has no fan path at all. The layouts below are transcribed from
 * that code; the section-22 one is additionally confirmed by measurement on
 * the NUC11PAxi7 this was written on, where sampling an idle ->
 * all-threads-busy -> cooldown cycle showed the 0x1D/0x1E pair (record 0's
 * speed) as the only value in EC RAM that moved like a fan.
 *
 * PICKING A LAYOUT
 *
 * Intel resolves the version from the BIOS string first and falls back to a WMI
 * PlatformCapabilityReport call whose result bits select ECNUC21/22/23. Both
 * are implemented here, but in this order:
 *
 *	1. layout= on the command line, if given
 *	2. the DMI table below - boards measured locally, which also unlocks
 *	   the temperature channels
 *	3. Intel's BIOS-string rules
 *	4. the WMI capability report, but only with wmi_probe=1
 *
 * Step 4 is off by default on purpose. It is transcribed faithfully from
 * Intel's GetCurrentECVersion(), but it could not be tested anywhere except
 * this one board - where steps 2 and 3 answer first anyway, so the WMI path
 * never runs unless it is asked for. Enabling it on an untested machine and
 * getting a wrong answer means silently reporting the wrong bytes as fan
 * speeds, so it stays opt-in until somebody has a board to check it against.
 *
 * The WMI details come from the binary MOF in the firmware:
 *
 *	[WMI, guid("{8C5DA44C-CDC3-46b3-8619-4E26D34390B7}")]
 *	class CISD_WMI {
 *	  [WmiMethodId(1537), Description("PlatformCapabilityReport")]
 *	  void PlatformCapabilityReport([in] uint32 Cmd, [out] CISD_Package Data);
 *	};
 *	class CISD_Package { uint8 Bytes[256]; };
 *
 * Cmd is 1 (the capability query). Byte 0 of the reply is a status - 0 means
 * no error - and byte 1 carries the version bits.
 *
 * The EC RAM identifies itself - bytes 0x00..0x05 are the ASCII "NUC_EC" - and
 * that is verified at probe whichever way the layout was chosen.
 *
 * TEMPERATURES
 *
 * Unlike the fan block, the temperature channels come only from local
 * measurement, with no vendor code to corroborate them, so they are published
 * only on a board whose DMI entry is in the table below. Only temp1 is
 * confidently identified - it tracks the coretemp package reading closely -
 * and the rest are labelled by EC address rather than given invented names.
 *
 * The driver never writes to the EC. There is no fan control here.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/wmi.h>

#define DRVNAME			"nuc_ec_hwmon"

#define NUC_EC_SIG		"NUC_EC"
#define NUC_EC_SIGLEN		6

#define NUC_EC_FAN_COUNT	6
#define NUC_EC_FAN_STRIDE	3
#define NUC_EC_FAN_BLOCK_LEN	(NUC_EC_FAN_COUNT * NUC_EC_FAN_STRIDE)

#define NUC_EC_TEMP_COUNT	5

/*
 * How long a sample stays good for. Every byte here costs an ACPI EC
 * transaction, which is slow and serialises against the firmware's own EC
 * traffic, so an unthrottled `sensors` run would issue 23 of them and a
 * `watch sensors` would do that every two seconds forever.
 */
#define NUC_EC_CACHE_TIME	(HZ)

/*
 * Absurdity limits. They do not validate the layout - a wrong byte can still
 * land inside these ranges - they only stop the impossible reaching userspace.
 */
#define NUC_EC_RPM_MAX		15000
#define NUC_EC_TEMP_MAX		127

/* One contiguous run of EC addresses contributing to the fan block. */
struct nuc_ec_seg {
	u8 addr;
	u8 len;
};

struct nuc_ec_layout {
	const char *name;
	struct nuc_ec_seg seg[2];
	u8 pad;			/* zero bytes appended to reach 18 */
};

static const struct nuc_ec_layout nuc_ec_layout_21 = {
	.name = "21", .seg = { { 0x1A, 9 }, { 0x37, 9 } }, .pad = 0,
};

static const struct nuc_ec_layout nuc_ec_layout_22 = {
	.name = "22", .seg = { { 0x1C, 18 }, { 0, 0 } }, .pad = 0,
};

static const struct nuc_ec_layout nuc_ec_layout_bc = {
	.name = "BC", .seg = { { 0x1B, 8 }, { 0x37, 9 } }, .pad = 1,
};

static char *layout;
module_param(layout, charp, 0444);
MODULE_PARM_DESC(layout,
		 "EC fan block layout: auto (default), 21, 22 or bc");

static bool wmi_probe;
module_param(wmi_probe, bool, 0444);
MODULE_PARM_DESC(wmi_probe,
		 "Fall back to Intel's WMI PlatformCapabilityReport to pick a layout (untested, off by default)");

/* CISD_WMI, from the firmware's binary MOF - see the header comment. */
#define NUC_EC_WMI_GUID		"8C5DA44C-CDC3-46B3-8619-4E26D34390B7"
#define NUC_EC_WMI_METHOD_CAP	1537	/* WmiMethodId of PlatformCapabilityReport */
#define NUC_EC_WMI_CMD_CAP	1	/* Cmd: capability query */
#define NUC_EC_WMI_MIN_LEN	54	/* Intel requires at least this much back */
#define NUC_EC_WMI_NO_ERROR	0	/* byte 0 of the reply */

/* Intel's FanType enumeration, from PerformanceTuningLibrary. */
enum nuc_ec_fan_type {
	NUC_EC_FAN_NONE		= 0,
	NUC_EC_FAN_CPU		= 1,
	NUC_EC_FAN_DGPU		= 2,
	NUC_EC_FAN_SYS1		= 3,
	NUC_EC_FAN_SYS2		= 4,
	NUC_EC_FAN_PUMP		= 5,
	NUC_EC_FAN_UNSUPPORTED	= 6,
	NUC_EC_FAN_CPU2		= 7,
};

static const char *nuc_ec_fan_label(u8 type)
{
	switch (type) {
	case NUC_EC_FAN_CPU:	return "CPU fan";
	case NUC_EC_FAN_DGPU:	return "dGPU fan";
	case NUC_EC_FAN_SYS1:	return "System fan 1";
	case NUC_EC_FAN_SYS2:	return "System fan 2";
	case NUC_EC_FAN_PUMP:	return "Liquid cooler pump";
	case NUC_EC_FAN_CPU2:	return "CPU fan 2";
	default:		return "Unknown fan";
	}
}

static bool nuc_ec_fan_present(u8 type)
{
	return type != NUC_EC_FAN_NONE && type != NUC_EC_FAN_UNSUPPORTED;
}

struct nuc_ec {
	const struct nuc_ec_layout *layout;
	bool have_temps;
	u8 fan_type[NUC_EC_FAN_COUNT];	/* settled at probe, for is_visible */

	/*
	 * The sample cache. An earlier version of this driver carried a mutex
	 * around bare register reads, where it protected nothing and implied an
	 * atomicity it could not deliver; that one was removed. This one guards
	 * real shared state.
	 */
	struct mutex lock;		/* guards the four fields below */
	unsigned long last_update;
	bool valid;
	u8 fan_block[NUC_EC_FAN_BLOCK_LEN];
	u8 temp[NUC_EC_TEMP_COUNT];
};

/* Empirical, this board only - see the header comment. */
static const u8 nuc_ec_temp_addr[NUC_EC_TEMP_COUNT] = {
	0x0E, 0x0F, 0x11, 0x13, 0x15,
};

static const char * const nuc_ec_temp_label[NUC_EC_TEMP_COUNT] = {
	"CPU", //  0x0E
	"EC 0x0F",
	"EC 0x11",
	"EC 0x13",
	"EC 0x15",
};

/* Fetch byte @idx of the assembled 18-byte fan block. */
static int nuc_ec_block_byte(const struct nuc_ec_layout *l, int idx, u8 *val)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(l->seg); i++) {
		if (idx < l->seg[i].len)
			return ec_read(l->seg[i].addr + idx, val);
		idx -= l->seg[i].len;
	}

	if (idx < l->pad) {
		*val = 0;
		return 0;
	}

	return -EINVAL;
}

/*
 * Refresh the cache if it has aged out. Caller holds ec->lock.
 *
 * The whole block is re-read in one pass rather than per attribute: a
 * `sensors` run touches a dozen files, and doing an EC transaction for each
 * would both be slow and hold the EC away from the firmware for longer than
 * necessary.
 */
static int nuc_ec_refresh(struct nuc_ec *ec)
{
	int i, err;

	if (ec->valid && time_before(jiffies, ec->last_update + NUC_EC_CACHE_TIME))
		return 0;

	for (i = 0; i < NUC_EC_FAN_BLOCK_LEN; i++) {
		err = nuc_ec_block_byte(ec->layout, i, &ec->fan_block[i]);
		if (err)
			goto stale;
	}

	/* Skipped entirely on boards where the temperature bytes are unknown. */
	if (ec->have_temps) {
		for (i = 0; i < NUC_EC_TEMP_COUNT; i++) {
			err = ec_read(nuc_ec_temp_addr[i], &ec->temp[i]);
			if (err)
				goto stale;
		}
	}

	ec->last_update = jiffies;
	ec->valid = true;
	return 0;

stale:
	ec->valid = false;
	return err;
}

static umode_t nuc_ec_is_visible(const void *data, enum hwmon_sensor_types type,
				 u32 attr, int channel)
{
	const struct nuc_ec *ec = data;

	switch (type) {
	case hwmon_fan:
		/* Hide the fan slots this board does not populate. */
		if (channel < 0 || channel >= NUC_EC_FAN_COUNT)
			return 0;
		if (!nuc_ec_fan_present(ec->fan_type[channel]))
			return 0;
		return 0444;

	case hwmon_temp:
		/* Only on boards whose temperature bytes were measured. */
		return ec->have_temps ? 0444 : 0;

	default:
		return 0;
	}
}

static int nuc_ec_read(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long *val)
{
	struct nuc_ec *ec = dev_get_drvdata(dev);
	u8 raw = 0, ftype = 0, hi = 0, lo = 0;
	unsigned int rpm;
	int err, base;

	mutex_lock(&ec->lock);
	err = nuc_ec_refresh(ec);
	if (!err) {
		switch (type) {
		case hwmon_temp:
			if (channel < 0 || channel >= NUC_EC_TEMP_COUNT)
				err = -EINVAL;
			else
				raw = ec->temp[channel];
			break;

		case hwmon_fan:
			if (channel < 0 || channel >= NUC_EC_FAN_COUNT) {
				err = -EINVAL;
			} else {
				base = channel * NUC_EC_FAN_STRIDE;
				ftype = ec->fan_block[base];
				hi = ec->fan_block[base + 1];
				lo = ec->fan_block[base + 2];
			}
			break;

		default:
			err = -EOPNOTSUPP;
			break;
		}
	}
	mutex_unlock(&ec->lock);

	if (err)
		return err;

	switch (type) {
	case hwmon_temp:
		/* 0x00 and 0xFF are how this EC reports "no reading". */
		if (raw == 0x00 || raw == 0xFF || raw > NUC_EC_TEMP_MAX)
			return -ENODATA;

		*val = raw * 1000;
		return 0;

	case hwmon_fan:
		if (!nuc_ec_fan_present(ftype))
			return -ENODATA;

		rpm = (hi << 8) | lo;
		if (rpm == 0xFFFF || rpm > NUC_EC_RPM_MAX)
			return -ENODATA;

		*val = rpm;
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

static int nuc_ec_read_string(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, const char **str)
{
	struct nuc_ec *ec = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		if (channel < 0 || channel >= NUC_EC_TEMP_COUNT)
			return -EINVAL;
		*str = nuc_ec_temp_label[channel];
		return 0;

	case hwmon_fan:
		if (channel < 0 || channel >= NUC_EC_FAN_COUNT)
			return -EINVAL;
		*str = nuc_ec_fan_label(ec->fan_type[channel]);
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops nuc_ec_hwmon_ops = {
	.is_visible	= nuc_ec_is_visible,
	.read		= nuc_ec_read,
	.read_string	= nuc_ec_read_string,
};

static const struct hwmon_channel_info * const nuc_ec_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_chip_info nuc_ec_chip_info = {
	.ops	= &nuc_ec_hwmon_ops,
	.info	= nuc_ec_info,
};

/*
 * Boards measured locally. Being listed here means both the fan layout and the
 * temperature addresses are trusted; a board matched only by BIOS string gets
 * fans but no temperatures.
 */
static const struct dmi_system_id nuc_ec_dmi_table[] = {
	{
		.ident = "Intel NUC11PAxi7",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Intel Corporation"),
			DMI_MATCH(DMI_BOARD_NAME, "NUC11PABi7"),
		},
		.driver_data = (void *)&nuc_ec_layout_22,
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, nuc_ec_dmi_table);

/* Intel's own BIOS-string rules, from PerformanceTuningLibrary. */
static const struct {
	const char *bios;
	const struct nuc_ec_layout *layout;
} nuc_ec_bios_table[] = {
	{ "EBTGL", &nuc_ec_layout_21 },
	{ "BCTGL", &nuc_ec_layout_bc },
	{ "HBADL", &nuc_ec_layout_21 },
};

static const struct nuc_ec_layout *nuc_ec_layout_by_name(const char *name)
{
	static const struct nuc_ec_layout * const all[] = {
		&nuc_ec_layout_21, &nuc_ec_layout_22, &nuc_ec_layout_bc,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(all); i++)
		if (!strcasecmp(name, all[i]->name))
			return all[i];

	return NULL;
}

/*
 * The WMI device, if wmi_probe was asked for and the firmware has it. The WMI
 * driver is registered before the platform driver in nuc_ec_init(), and driver
 * registration walks already-present devices synchronously, so this is settled
 * by the time any platform probe runs.
 */
static struct wmi_device *nuc_ec_wmi_dev;

static int nuc_ec_wmi_probe(struct wmi_device *wdev, const void *context)
{
	nuc_ec_wmi_dev = wdev;
	return 0;
}

static void nuc_ec_wmi_remove(struct wmi_device *wdev)
{
	nuc_ec_wmi_dev = NULL;
}

static const struct wmi_device_id nuc_ec_wmi_ids[] = {
	{ NUC_EC_WMI_GUID, NULL },
	{ }
};

/*
 * No MODULE_DEVICE_TABLE(wmi, ...) on purpose: this GUID is Intel's general
 * NUC interface and lives on machines this driver has no business touching.
 * Autoloading on it would be noise. The table is only here so the driver can
 * bind once nuc_ec_init() explicitly registers it.
 */

static struct wmi_driver nuc_ec_wmi_driver = {
	.driver = {
		.name = DRVNAME,
	},
	.id_table = nuc_ec_wmi_ids,
	.probe = nuc_ec_wmi_probe,
	.remove = nuc_ec_wmi_remove,
};

/* Byte 1 of PlatformCapabilityReport's reply. */
static int nuc_ec_wmi_capability(struct device *dev, u8 *caps)
{
	u32 cmd = NUC_EC_WMI_CMD_CAP;
	struct acpi_buffer in = { sizeof(cmd), &cmd };
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int ret = 0;

	if (!nuc_ec_wmi_dev)
		return -ENODEV;

	status = wmidev_evaluate_method(nuc_ec_wmi_dev, 0, NUC_EC_WMI_METHOD_CAP,
					&in, &out);
	if (ACPI_FAILURE(status))
		return -EIO;

	obj = out.pointer;
	if (!obj)
		return -ENODATA;

	if (obj->type != ACPI_TYPE_BUFFER ||
	    obj->buffer.length < NUC_EC_WMI_MIN_LEN) {
		ret = -ENODATA;
		goto out;
	}

	if (obj->buffer.pointer[0] != NUC_EC_WMI_NO_ERROR) {
		dev_info(dev, "capability report returned status %u\n",
			 obj->buffer.pointer[0]);
		ret = -EIO;
		goto out;
	}

	*caps = obj->buffer.pointer[1];
out:
	kfree(obj);
	return ret;
}

/*
 * Transcribed from Intel's GetCurrentECVersion(). The branch order matters and
 * is theirs, including the fact that bit 1 set without bit 0 reaches the
 * ECElement test rather than ECNUC21.
 */
static const struct nuc_ec_layout *nuc_ec_layout_from_wmi(struct device *dev)
{
	const struct nuc_ec_layout *l;
	const char *version;
	u8 caps;
	int err;

	err = nuc_ec_wmi_capability(dev, &caps);
	if (err) {
		dev_info(dev, "WMI capability report unavailable (%d)\n", err);
		return NULL;
	}

	if ((caps & BIT(0)) && (caps & BIT(1))) {
		version = "ECNUC23";
		l = &nuc_ec_layout_22;
	} else if (caps & BIT(0)) {
		version = "ECNUC22";
		l = &nuc_ec_layout_22;
	} else if (!(caps & BIT(0)) && !(caps & BIT(1))) {
		version = "ECNUC21";
		l = &nuc_ec_layout_21;
	} else if (caps & BIT(2)) {
		version = "ECElement";
		l = NULL;	/* Intel has no fan path for this one */
	} else {
		version = "Reserve";
		l = NULL;
	}

	dev_info(dev, "WMI capability bits 0x%02x -> %s%s%s\n", caps, version,
		 l ? ", layout " : " (no fan support)", l ? l->name : "");

	return l;
}

static const struct nuc_ec_layout *nuc_ec_pick_layout(struct device *dev,
						      bool *have_temps)
{
	const struct dmi_system_id *id;
	const char *bios;
	int i;

	*have_temps = false;

	if (layout && strcmp(layout, "auto")) {
		const struct nuc_ec_layout *l = nuc_ec_layout_by_name(layout);

		if (!l)
			dev_err(dev, "unknown layout=%s\n", layout);
		else
			dev_info(dev, "using layout %s (forced)\n", l->name);
		return l;
	}

	id = dmi_first_match(nuc_ec_dmi_table);
	if (id) {
		*have_temps = true;
		return id->driver_data;
	}

	bios = dmi_get_system_info(DMI_BIOS_VERSION);
	if (bios) {
		for (i = 0; i < ARRAY_SIZE(nuc_ec_bios_table); i++) {
			if (strstr(bios, nuc_ec_bios_table[i].bios)) {
				dev_info(dev, "BIOS %s matches %s, using layout %s\n",
					 bios, nuc_ec_bios_table[i].bios,
					 nuc_ec_bios_table[i].layout->name);
				return nuc_ec_bios_table[i].layout;
			}
		}
	}

	if (wmi_probe)
		return nuc_ec_layout_from_wmi(dev);

	return NULL;
}

static int nuc_ec_check_signature(struct device *dev)
{
	char sig[NUC_EC_SIGLEN];
	int i, err;
	u8 v;

	for (i = 0; i < NUC_EC_SIGLEN; i++) {
		err = ec_read(i, &v);
		if (err)
			return dev_err_probe(dev, err, "EC read failed\n");
		sig[i] = v;
	}

	if (memcmp(sig, NUC_EC_SIG, NUC_EC_SIGLEN))
		return dev_err_probe(dev, -ENODEV,
				     "EC does not carry the %s signature\n",
				     NUC_EC_SIG);
	return 0;
}

static int nuc_ec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *hwmon;
	struct nuc_ec *ec;
	int i, err, nfan = 0;

	ec = devm_kzalloc(dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;

	mutex_init(&ec->lock);

	ec->layout = nuc_ec_pick_layout(dev, &ec->have_temps);
	if (!ec->layout)
		return dev_err_probe(dev, -ENODEV,
				     "no known EC layout for this board; try layout=21, 22 or bc\n");

	err = nuc_ec_check_signature(dev);
	if (err)
		return err;

	/*
	 * Populate the cache once here so is_visible() knows which fan slots
	 * this board actually fills. Which fans exist is treated as fixed at
	 * probe: hwmon decides attribute visibility once, and a NUC does not
	 * gain a fan while running.
	 */
	mutex_lock(&ec->lock);
	err = nuc_ec_refresh(ec);
	mutex_unlock(&ec->lock);
	if (err)
		return dev_err_probe(dev, err, "EC read failed\n");

	for (i = 0; i < NUC_EC_FAN_COUNT; i++) {
		ec->fan_type[i] = ec->fan_block[i * NUC_EC_FAN_STRIDE];
		if (nuc_ec_fan_present(ec->fan_type[i]))
			nfan++;
	}

	hwmon = devm_hwmon_device_register_with_info(dev, "nuc_ec", ec,
						     &nuc_ec_chip_info, NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	dev_info(dev, "Intel NUC EC (layout %s): %d fan(s), %d temperature channel(s)\n",
		 ec->layout->name, nfan,
		 ec->have_temps ? NUC_EC_TEMP_COUNT : 0);

	return 0;
}

static const struct acpi_device_id nuc_ec_acpi_ids[] = {
	{ "INTC1036", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, nuc_ec_acpi_ids);

static struct platform_driver nuc_ec_driver = {
	.driver = {
		.name = DRVNAME,
		.acpi_match_table = nuc_ec_acpi_ids,
	},
	.probe = nuc_ec_probe,
};

static bool nuc_ec_wmi_registered;

static int __init nuc_ec_init(void)
{
	int err;

	/*
	 * Register the WMI driver first when it is wanted: driver registration
	 * binds already-present devices synchronously, so nuc_ec_wmi_dev is
	 * settled before the platform driver can probe and no deferral dance
	 * is needed. Failing to bind is not fatal - the platform probe only
	 * consults it after the DMI and BIOS-string rules have both missed.
	 */
	if (wmi_probe) {
		err = wmi_driver_register(&nuc_ec_wmi_driver);
		if (err)
			pr_warn("could not register WMI driver (%d); continuing without it\n",
				err);
		else
			nuc_ec_wmi_registered = true;
	}

	err = platform_driver_register(&nuc_ec_driver);
	if (err && nuc_ec_wmi_registered) {
		wmi_driver_unregister(&nuc_ec_wmi_driver);
		nuc_ec_wmi_registered = false;
	}

	return err;
}

static void __exit nuc_ec_exit(void)
{
	platform_driver_unregister(&nuc_ec_driver);

	if (nuc_ec_wmi_registered)
		wmi_driver_unregister(&nuc_ec_wmi_driver);
}

module_init(nuc_ec_init);
module_exit(nuc_ec_exit);

MODULE_AUTHOR("Cam Cope");
MODULE_DESCRIPTION("hwmon driver for the Intel NUC embedded controller");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
