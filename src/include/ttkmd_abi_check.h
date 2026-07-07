// Maps to: docs/abi-ground-truth.txt (generated from tt-kmd/ioctl.h by gcc).
//
// Compile-time enforcement that the Windows ABI structs are byte-identical to
// the Linux ones. Include from at least one driver TU and one user-mode test TU.
// Values are copied verbatim from docs/abi-ground-truth.txt (baseline
// ttkmd-2.10.0-rc1-1-g8c32c2b); regenerate with tools/gen_abi_truth.sh on every
// upstream rebase and update here (maintenance guide step 2).

#ifndef TTKMD_ABI_CHECK_H_INCLUDED
#define TTKMD_ABI_CHECK_H_INCLUDED

#include <stddef.h>
#include "ttkmd_ioctl.h"

#define TT_ABI_SIZE(t, n) \
    static_assert(sizeof(struct t) == (n), #t " size != Linux ABI")
#define TT_ABI_FIELD(t, f, off, sz) \
    static_assert(offsetof(struct t, f) == (off) && sizeof(((struct t *)0)->f) == (sz), \
                  #t "." #f " layout != Linux ABI")

// GET_DEVICE_INFO (nr 0)
TT_ABI_SIZE(tenstorrent_get_device_info_in, 4);
TT_ABI_FIELD(tenstorrent_get_device_info_in, output_size_bytes, 0, 4);
TT_ABI_SIZE(tenstorrent_get_device_info_out, 20);
TT_ABI_FIELD(tenstorrent_get_device_info_out, output_size_bytes, 0, 4);
TT_ABI_FIELD(tenstorrent_get_device_info_out, vendor_id, 4, 2);
TT_ABI_FIELD(tenstorrent_get_device_info_out, device_id, 6, 2);
TT_ABI_FIELD(tenstorrent_get_device_info_out, subsystem_vendor_id, 8, 2);
TT_ABI_FIELD(tenstorrent_get_device_info_out, subsystem_id, 10, 2);
TT_ABI_FIELD(tenstorrent_get_device_info_out, bus_dev_fn, 12, 2);
TT_ABI_FIELD(tenstorrent_get_device_info_out, max_dma_buf_size_log2, 14, 2);
TT_ABI_FIELD(tenstorrent_get_device_info_out, pci_domain, 16, 2);
TT_ABI_FIELD(tenstorrent_get_device_info_out, reserved, 18, 2);
TT_ABI_SIZE(tenstorrent_get_device_info, 24);
TT_ABI_FIELD(tenstorrent_get_device_info, out, 4, 20);

// QUERY_MAPPINGS (nr 2)
TT_ABI_SIZE(tenstorrent_query_mappings_in, 8);
TT_ABI_FIELD(tenstorrent_query_mappings_in, output_mapping_count, 0, 4);
TT_ABI_FIELD(tenstorrent_query_mappings_in, reserved, 4, 4);
TT_ABI_SIZE(tenstorrent_mapping, 24);
TT_ABI_FIELD(tenstorrent_mapping, mapping_id, 0, 4);
TT_ABI_FIELD(tenstorrent_mapping, reserved, 4, 4);
TT_ABI_FIELD(tenstorrent_mapping, mapping_base, 8, 8);
TT_ABI_FIELD(tenstorrent_mapping, mapping_size, 16, 8);
TT_ABI_SIZE(tenstorrent_query_mappings, 8);

// GET_DRIVER_INFO (nr 5)
TT_ABI_SIZE(tenstorrent_get_driver_info_in, 4);
TT_ABI_SIZE(tenstorrent_get_driver_info_out, 12);
TT_ABI_FIELD(tenstorrent_get_driver_info_out, output_size_bytes, 0, 4);
TT_ABI_FIELD(tenstorrent_get_driver_info_out, driver_version, 4, 4);
TT_ABI_FIELD(tenstorrent_get_driver_info_out, driver_version_major, 8, 1);
TT_ABI_FIELD(tenstorrent_get_driver_info_out, driver_version_minor, 9, 1);
TT_ABI_FIELD(tenstorrent_get_driver_info_out, driver_version_patch, 10, 1);
TT_ABI_FIELD(tenstorrent_get_driver_info_out, reserved0, 11, 1);
TT_ABI_SIZE(tenstorrent_get_driver_info, 16);
TT_ABI_FIELD(tenstorrent_get_driver_info, out, 4, 12);

// CTL_CODE values fixed by DD-4; a change here is an ABI break for user mode.
static_assert(IOCTL_TENSTORRENT_GET_DEVICE_INFO == 0x80FA2000, "CTL code drift");
static_assert(IOCTL_TENSTORRENT_QUERY_MAPPINGS == 0x80FA2008, "CTL code drift");
static_assert(IOCTL_TENSTORRENT_GET_DRIVER_INFO == 0x80FA2014, "CTL code drift");
static_assert(IOCTL_TENSTORRENT_EXPORT_TLB_DMABUF == 0x80FA2040, "CTL code drift");

#undef TT_ABI_SIZE
#undef TT_ABI_FIELD

#endif // TTKMD_ABI_CHECK_H_INCLUDED
