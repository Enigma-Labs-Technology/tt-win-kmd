// Windows-only DEBUG ioctls (no Linux equivalent; spec mapping row
// "debugfs/procfs -> debug-only IOCTLs gated behind a build flag").
//
// Compiled into the driver only when TT_DEBUG_INTERFACES is defined (set in
// ttkmd.vcxproj for development builds; must be removed for any release/
// attestation build — see docs/signing-and-deployment.md). Function codes use
// the 0xA00 range to stay clear of Linux-parity (0x800) and Windows-extension
// (0x900) code space (parity matrix).

#ifndef TTKMD_DEBUG_H_INCLUDED
#define TTKMD_DEBUG_H_INCLUDED

#include "ttkmd_ioctl.h"

#define TT_CTL_DEBUG(nr) \
    CTL_CODE(TT_DEVICE_TYPE, 0xA00u + (nr), METHOD_BUFFERED, FILE_ANY_ACCESS)

// Read one telemetry tag (maps to tt-kmd read_telemetry_tag semantics:
// tag >= 128 -> STATUS_INVALID_PARAMETER; tag absent -> STATUS_NOT_FOUND).
#define IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY TT_CTL_DEBUG(0)

struct tenstorrent_debug_read_telemetry {
    uint32_t tag_id;    // in
    uint32_t value;     // out
};

// Synchronous ARC message round-trip (send_arc_message parity). The struct is
// overwritten with the response; 'success' mirrors the Linux bool (response
// header == 0). Ioctl status is success even when 'success' is 0, matching
// how Linux callers see a false return rather than an errno.
#define IOCTL_TENSTORRENT_DEBUG_ARC_MSG TT_CTL_DEBUG(1)

struct tenstorrent_debug_arc_msg {
    uint32_t header;      // in: request header / out: response header
    uint32_t payload[7];  // in/out
    uint32_t success;     // out
};

#endif // TTKMD_DEBUG_H_INCLUDED
