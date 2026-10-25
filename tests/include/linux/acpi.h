/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal ACPICA boundary declarations for the userspace reader tests. */
#ifndef TEST_LINUX_ACPI_H
#define TEST_LINUX_ACPI_H

#include <stdint.h>

typedef void *acpi_handle;
typedef uint32_t acpi_status;
typedef uint32_t acpi_object_type;
struct acpi_object_list;

#define AE_OK 0
#define AE_ERROR 1
#define AE_NOT_FOUND 5
#define ACPI_FAILURE(status) ((status) != AE_OK)
#define ACPI_TYPE_INTEGER 0x01
#define ACPI_TYPE_METHOD 0x08
#define ACPI_TYPE_LOCAL_REGION_FIELD 0x11
#define ACPI_TYPE_LOCAL_BANK_FIELD 0x12
#define ACPI_TYPE_LOCAL_INDEX_FIELD 0x13

acpi_status acpi_get_handle(acpi_handle parent, char *path, acpi_handle *out);
acpi_status acpi_get_type(acpi_handle handle, acpi_object_type *out);
acpi_status acpi_evaluate_integer(acpi_handle handle, char *path,
				struct acpi_object_list *args,
				unsigned long long *out);

#endif
