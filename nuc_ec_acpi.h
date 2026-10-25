/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NUC_EC_ACPI_H
#define NUC_EC_ACPI_H

#include <linux/acpi.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>

static int nuc_ec_tn_read_byte(u8 addr, u8 *val)
{
	unsigned long long raw;
	acpi_object_type type;
	acpi_handle handle;
	acpi_status status;
	char path[16];

	/* Only the signature and layout-22 fan records have been validated. */
	if (addr > 0x05 && (addr < 0x1c || addr > 0x2d))
		return -EINVAL;

	snprintf(path, sizeof(path), "\\_SB.R%03X", 0x400 + addr);
	status = acpi_get_handle(NULL, path, &handle);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	/* An integer result alone would not prevent invoking a Method object. */
	status = acpi_get_type(handle, &type);
	if (ACPI_FAILURE(status))
		return -EIO;
	if (type != ACPI_TYPE_LOCAL_REGION_FIELD)
		return -EINVAL;

	status = acpi_evaluate_integer(handle, NULL, NULL, &raw);
	if (ACPI_FAILURE(status))
		return -EIO;
	if (raw > 0xff)
		return -ENODATA;

	*val = raw;
	return 0;
}

#endif
