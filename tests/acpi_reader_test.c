/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production reader; only the kernel/firmware boundary is fake. */
#include <assert.h>
#include <limits.h>
#include <string.h>

#include "nuc_ec_acpi.h"

static struct {
	acpi_status lookup_status;
	acpi_status type_status;
	acpi_status read_status;
	acpi_object_type type;
	unsigned long long value;
	unsigned int lookups;
	unsigned int reads;
	char path[32];
} firmware;
static int field_cookie;

static void reset_firmware(void)
{
	memset(&firmware, 0, sizeof(firmware));
	firmware.type = ACPI_TYPE_LOCAL_REGION_FIELD;
	firmware.value = 0xa5;
}

acpi_status acpi_get_handle(acpi_handle parent, char *path, acpi_handle *out)
{
	assert(parent == NULL);
	firmware.lookups++;
	snprintf(firmware.path, sizeof(firmware.path), "%s", path);
	*out = &field_cookie;
	return firmware.lookup_status;
}

acpi_status acpi_get_type(acpi_handle handle, acpi_object_type *out)
{
	assert(handle == &field_cookie);
	*out = firmware.type;
	return firmware.type_status;
}

acpi_status acpi_evaluate_integer(acpi_handle handle, char *path,
				struct acpi_object_list *args,
				unsigned long long *out)
{
	/* The checked handle must be used, not another absolute-name lookup. */
	assert(handle == &field_cookie);
	assert(path == NULL);
	assert(args == NULL);
	firmware.reads++;
	*out = firmware.value;
	return firmware.read_status;
}

static void test_fixed_signature_and_fan_paths(void)
{
	static const struct {
		u8 addr;
		const char *path;
	} cases[] = {
		{ 0x00, "\\_SB.R400" },
		{ 0x05, "\\_SB.R405" },
		{ 0x1c, "\\_SB.R41C" },
		{ 0x1d, "\\_SB.R41D" },
		{ 0x1e, "\\_SB.R41E" },
		{ 0x2d, "\\_SB.R42D" },
	};
	unsigned int i;
	u8 value;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		reset_firmware();
		value = 0xcc;
		assert(nuc_ec_tn_read_byte(cases[i].addr, &value) == 0);
		assert(value == 0xa5);
		assert(strcmp(firmware.path, cases[i].path) == 0);
	}
}

static void test_all_other_addresses_are_rejected_without_firmware_access(void)
{
	unsigned int addr;
	u8 value;
	int result;

	for (addr = 0; addr <= 0xff; addr++) {
		reset_firmware();
		value = 0xcc;
		result = nuc_ec_tn_read_byte(addr, &value);
		if (addr <= 0x05 || (addr >= 0x1c && addr <= 0x2d)) {
			assert(result == 0);
			assert(value == 0xa5);
		} else {
			assert(result == -EINVAL);
			assert(value == 0xcc);
			assert(firmware.lookups == 0);
			assert(firmware.reads == 0);
		}
	}
}

static void test_missing_fields_fail_without_publishing_a_byte(void)
{
	u8 value = 0xcc;

	reset_firmware();
	firmware.lookup_status = AE_NOT_FOUND;
	assert(nuc_ec_tn_read_byte(0x1d, &value) == -ENODEV);
	assert(value == 0xcc);
	assert(firmware.reads == 0);
}

static void test_type_query_failure_does_not_evaluate_the_object(void)
{
	u8 value = 0xcc;

	reset_firmware();
	firmware.type_status = AE_ERROR;
	assert(nuc_ec_tn_read_byte(0x1d, &value) == -EIO);
	assert(value == 0xcc);
	assert(firmware.reads == 0);
}

static void test_methods_and_other_object_types_are_never_evaluated(void)
{
	const acpi_object_type types[] = {
		ACPI_TYPE_METHOD, ACPI_TYPE_INTEGER,
		ACPI_TYPE_LOCAL_BANK_FIELD, ACPI_TYPE_LOCAL_INDEX_FIELD,
	};
	unsigned int i;
	u8 value;

	for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		reset_firmware();
		firmware.type = types[i];
		value = 0xcc;
		assert(nuc_ec_tn_read_byte(0x1d, &value) == -EINVAL);
		assert(value == 0xcc);
		assert(firmware.reads == 0);
	}
}

static void test_read_failure_is_not_published_as_zero(void)
{
	u8 value = 0xcc;

	reset_firmware();
	firmware.read_status = AE_ERROR;
	assert(nuc_ec_tn_read_byte(0x1d, &value) == -EIO);
	assert(value == 0xcc);
}

static void test_values_wider_than_a_byte_are_rejected(void)
{
	const unsigned long long values[] = { 0x100, ULLONG_MAX };
	unsigned int i;
	u8 value;

	for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		reset_firmware();
		firmware.value = values[i];
		value = 0xcc;
		assert(nuc_ec_tn_read_byte(0x1d, &value) == -ENODATA);
		assert(value == 0xcc);
	}
}

static void test_zero_and_maximum_byte_are_valid_data(void)
{
	const unsigned long long values[] = { 0, 0xff };
	unsigned int i;
	u8 value;

	for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		reset_firmware();
		firmware.value = values[i];
		value = 0xcc;
		assert(nuc_ec_tn_read_byte(0x1d, &value) == 0);
		assert(value == values[i]);
	}
}

int main(void)
{
	test_fixed_signature_and_fan_paths();
	test_all_other_addresses_are_rejected_without_firmware_access();
	test_missing_fields_fail_without_publishing_a_byte();
	test_type_query_failure_does_not_evaluate_the_object();
	test_methods_and_other_object_types_are_never_evaluated();
	test_read_failure_is_not_published_as_zero();
	test_values_wider_than_a_byte_are_rejected();
	test_zero_and_maximum_byte_are_valid_data();
	puts("ACPI reader: 8 tests passed");
	return 0;
}
