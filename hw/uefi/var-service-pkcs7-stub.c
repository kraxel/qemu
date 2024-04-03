/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * uefi vars device - pkcs7 stubs
 */
#include "qemu/osdep.h"
#include "sysemu/dma.h"

#include "hw/uefi/var-service.h"

efi_status uefi_vars_check_pkcs7_2(uefi_variable *siglist,
                                   mm_variable_access *va, void *data)
{
    return EFI_WRITE_PROTECTED;
}
