// SPDX-FileCopyrightText: 2026 tt-win-kmd contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Maps to: tt-kmd/chardev.c ioctl dispatch (tt_cdev_ioctl, chardev.c:591-706)
// and the M1 handlers: ioctl_get_device_info (chardev.c:128-160),
// ioctl_get_driver_info, ioctl_query_mappings (memory.c:340-410).
//
// Argument-protocol mapping (analysis §04.3 → METHOD_BUFFERED): the caller
// passes the Linux struct as BOTH input and output buffer of DeviceIoControl.
// Linux -EFAULT (unreadable in / unwritable declared-out area) maps to
// STATUS_ACCESS_VIOLATION as "buffer too small for the declared area".
#include "ttkmd.h"

#include <stddef.h>
#include "ttkmd_ioctl.h"
#include "blackhole.h"
#ifdef TT_DEBUG_INTERFACES
#include "ttkmd_debug.h"
#endif

// Extracts the Linux ioctl nr (0..16) from a CTL_CODE built by TT_CTL (DD-4);
// returns 0xFFFFFFFF for foreign codes.
static UINT32
TtIoctlNr(
    _In_ ULONG IoControlCode
    )
{
    if ((IoControlCode & 0xFFFF0000u) != (TT_DEVICE_TYPE << 16) ||
        (IoControlCode & 0x3u) != METHOD_BUFFERED) {
        return 0xFFFFFFFFu;
    }
    return ((IoControlCode >> 2) & 0xFFFu) - 0x800u;
}

// Protocol 3a completion (chardev.c:151-157 parity): zero-fill the entire
// declared output area, copy min(declared, sizeof(out)) bytes of payload,
// report Information = outOffset + declared so the zero-fill reaches the user.
_Use_decl_annotations_
NTSTATUS
TtCompleteSizedOutBuffer(
    WDFREQUEST Request,
    size_t OutOffset,
    const VOID *Out,
    size_t OutSize,
    UINT32 Declared
    )
{
    PUCHAR buffer;
    size_t bufferLength;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputBuffer(Request, 0, (PVOID *)&buffer,
                                            &bufferLength);
    if (!NT_SUCCESS(status)) {
        return STATUS_ACCESS_VIOLATION;   // clear_user fault parity
    }
    if (bufferLength < OutOffset || bufferLength - OutOffset < Declared) {
        return STATUS_ACCESS_VIOLATION;   // declared area not writable
    }

    RtlZeroMemory(buffer + OutOffset, Declared);
    RtlCopyMemory(buffer + OutOffset, Out, min((size_t)Declared, OutSize));
    WdfRequestSetInformation(Request, OutOffset + Declared);
    return STATUS_SUCCESS;
}

// Fixed-size input retrieval: Linux copy_from_user(sizeof(in)) parity; a
// shorter input buffer is the -EFAULT case.
_Use_decl_annotations_
NTSTATUS
TtCopyInBuffer(
    WDFREQUEST Request,
    VOID *In,
    size_t InSize
    )
{
    PVOID buffer;
    size_t bufferLength;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(Request, InSize, &buffer,
                                           &bufferLength);
    if (!NT_SUCCESS(status)) {
        return STATUS_ACCESS_VIOLATION;
    }
    RtlCopyMemory(In, buffer, InSize);
    return STATUS_SUCCESS;
}

static NTSTATUS
TtIoctlGetDeviceInfo(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request
    )
{
    struct tenstorrent_get_device_info_in in;
    struct tenstorrent_get_device_info_out out;
    NTSTATUS status;

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&out, sizeof(out));
    out.output_size_bytes = sizeof(out);          // chardev.c:142
    out.vendor_id = Context->VendorId;
    out.device_id = Context->DeviceId;
    out.subsystem_vendor_id = Context->SubsysVendorId;
    out.subsystem_id = Context->SubsysId;
    out.bus_dev_fn = Context->BusDevFn;
    out.max_dma_buf_size_log2 = 28;               // MAX_DMA_BUF_SIZE_LOG2, memory.h:10
    out.pci_domain = Context->PciDomain;

    return TtCompleteSizedOutBuffer(Request,
                              offsetof(struct tenstorrent_get_device_info, out),
                              &out, sizeof(out), in.output_size_bytes);
}

static NTSTATUS
TtIoctlGetDriverInfo(
    _In_ WDFREQUEST Request
    )
{
    struct tenstorrent_get_driver_info_in in;
    struct tenstorrent_get_driver_info_out out;
    NTSTATUS status;

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&out, sizeof(out));
    out.output_size_bytes = sizeof(out);
    out.driver_version = TENSTORRENT_DRIVER_VERSION;
    out.driver_version_major = TT_VERSION_MAJOR;
    out.driver_version_minor = TT_VERSION_MINOR;
    out.driver_version_patch = TT_VERSION_PATCH;

    return TtCompleteSizedOutBuffer(Request,
                              offsetof(struct tenstorrent_get_driver_info, out),
                              &out, sizeof(out), in.output_size_bytes);
}

static NTSTATUS
TtIoctlQueryMappings(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request
    )
{
    // memory.c:340-410 parity: pack UC/WC pairs for BARs 0, 2, 4 that exist,
    // copy min(declared, valid) entries, zero-fill the remaining declared
    // slots. mapping_base values are the opaque Linux mmap-offset tokens.
    struct tenstorrent_query_mappings_in in;
    struct tenstorrent_mapping mappings[6];
    UINT32 valid = 0;
    UINT32 toCopy;
    UINT64 declaredBytes;
    PUCHAR buffer;
    size_t bufferLength;
    static const struct {
        ULONG bar;
        UINT32 idUc;
    } table[3] = {
        { 0, TENSTORRENT_MAPPING_RESOURCE0_UC },
        { 2, TENSTORRENT_MAPPING_RESOURCE1_UC },
        { 4, TENSTORRENT_MAPPING_RESOURCE2_UC },
    };
    ULONG i;
    NTSTATUS status;

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(mappings, sizeof(mappings));
    for (i = 0; i < ARRAYSIZE(table); i++) {
        UINT64 len = Context->BarLength[table[i].bar];

        if (len == 0) {
            continue;
        }
        // UC then WC; mapping_base = (mapping_id - 1) << 36 (memory.c:255-274:
        // MMAP_OFFSET_RESOURCE<n>_<UC/WC> tokens).
        mappings[valid].mapping_id = table[i].idUc;
        mappings[valid].mapping_base = ((UINT64)table[i].idUc - 1) << 36;
        mappings[valid].mapping_size = len;
        valid++;
        mappings[valid].mapping_id = table[i].idUc + 1;
        mappings[valid].mapping_base = ((UINT64)table[i].idUc) << 36;
        mappings[valid].mapping_size = len;
        valid++;
    }

    // Output area = mapping array immediately after the in struct.
    declaredBytes = (UINT64)in.output_mapping_count *
                    sizeof(struct tenstorrent_mapping);
    if (declaredBytes > MAXUINT32) {
        return STATUS_ACCESS_VIOLATION;   // U32_MAX guard parity, memory.c:398
    }

    status = WdfRequestRetrieveOutputBuffer(Request, 0, (PVOID *)&buffer,
                                            &bufferLength);
    if (!NT_SUCCESS(status)) {
        return STATUS_ACCESS_VIOLATION;
    }
    if (bufferLength < sizeof(struct tenstorrent_query_mappings_in) ||
        bufferLength - sizeof(struct tenstorrent_query_mappings_in) <
            declaredBytes) {
        return STATUS_ACCESS_VIOLATION;   // declared slots not writable
    }

    toCopy = min(in.output_mapping_count, valid);
    RtlZeroMemory(buffer + sizeof(struct tenstorrent_query_mappings_in),
                  (size_t)declaredBytes);
    RtlCopyMemory(buffer + sizeof(struct tenstorrent_query_mappings_in),
                  mappings, toCopy * sizeof(struct tenstorrent_mapping));
    WdfRequestSetInformation(Request,
                             sizeof(struct tenstorrent_query_mappings_in) +
                             (size_t)declaredBytes);
    return STATUS_SUCCESS;
}

// Gate order per tt_cdev_ioctl (chardev.c:591-624, analysis §04.2). Nr of
// MAXUINT32 marks mmap-path callers (MAP/UNMAP): Linux mmap checks detached
// and reset_gen but NOT needs_hw_init (documented asymmetry, analysis §11).
// The reset rwsem lands with RESET_DEVICE (M4).
_Use_decl_annotations_
NTSTATUS
TtCheckIoGates(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    UINT32 Nr
    )
{
    if (Context->Detached) {
        return STATUS_DEVICE_REMOVED;                 // -ENODEV
    }
    if (FileObject != NULL &&
        TtGetFileContext(FileObject)->OpenResetGen !=
            ReadAcquire64(&Context->ResetGen)) {
        return STATUS_DEVICE_REMOVED;                 // -ENODEV
    }
    if (Context->NeedsHwInit && Nr != MAXUINT32 &&
        Nr != 0 && Nr != 5 && Nr != 6) {
        // Post-reset window: only GET_DEVICE_INFO, GET_DRIVER_INFO,
        // RESET_DEVICE are allowed (chardev.c:616-624). QUERY_TELEMETRY maps
        // Linux's -ENODATA distinction (telemetry.c:23-24) to NOT_READY so a
        // telemetry poller sees a transient, not device-gone (porting note,
        // analysis §10:369).
        return (Nr == 0x102u) ? STATUS_DEVICE_NOT_READY
                              : STATUS_DEVICE_REMOVED; // -ENODATA / -ENODEV
    }
    return STATUS_SUCCESS;
}

#ifdef TT_DEBUG_INTERFACES
// Debug surface (ttkmd_debug.h): thin wrappers over the M2 Blackhole layer.
// Fixed-size in/out, no size negotiation (protocol 3d style).

static NTSTATUS
TtIoctlDebugReadTelemetry(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request
    )
{
    struct tenstorrent_debug_read_telemetry arg;
    PVOID outBuf;
    size_t outLen;
    UINT32 value = 0;
    NTSTATUS status;

    if (!Context->IsBlackhole || !Context->TelemetryValid) {
        return STATUS_NOT_SUPPORTED;
    }

    status = TtCopyInBuffer(Request, &arg, sizeof(arg));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (arg.tag_id > MAXUINT16) {
        return STATUS_INVALID_PARAMETER;
    }

    status = TtBhReadTelemetryTag(Context, (UINT16)arg.tag_id, &value);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    arg.value = value;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(arg), &outBuf,
                                            &outLen);
    if (!NT_SUCCESS(status) || outLen < sizeof(arg)) {
        return STATUS_ACCESS_VIOLATION;
    }
    RtlCopyMemory(outBuf, &arg, sizeof(arg));
    WdfRequestSetInformation(Request, sizeof(arg));
    return STATUS_SUCCESS;
}

static NTSTATUS
TtIoctlDebugArcMsg(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request
    )
{
    struct tenstorrent_debug_arc_msg arg;
    TT_ARC_MSG msg;
    PVOID outBuf;
    size_t outLen;
    NTSTATUS status;

    if (!Context->IsBlackhole) {
        return STATUS_NOT_SUPPORTED;
    }

    status = TtCopyInBuffer(Request, &arg, sizeof(arg));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    msg.Header = arg.header;
    RtlCopyMemory(msg.Payload, arg.payload, sizeof(msg.Payload));

    arg.success = TtBhSendArcMessage(Context, &msg) ? 1 : 0;
    arg.header = msg.Header;
    RtlCopyMemory(arg.payload, msg.Payload, sizeof(arg.payload));

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(arg), &outBuf,
                                            &outLen);
    if (!NT_SUCCESS(status) || outLen < sizeof(arg)) {
        return STATUS_ACCESS_VIOLATION;
    }
    RtlCopyMemory(outBuf, &arg, sizeof(arg));
    WdfRequestSetInformation(Request, sizeof(arg));
    return STATUS_SUCCESS;
}

static NTSTATUS
TtIoctlDebugGetAggPower(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ WDFREQUEST Request
    )
{
    struct tenstorrent_debug_agg_power out;
    PVOID outBuf;
    size_t outLen;
    NTSTATUS status;

    RtlZeroMemory(&out, sizeof(out));
    WdfWaitLockAcquire(Context->PowerLock, NULL);
    out.valid = Context->PowerAggValid ? 1 : 0;
    out.power_flags = Context->PowerAggFlags;
    RtlCopyMemory(out.power_settings, Context->PowerAggSettings,
                  sizeof(out.power_settings));
    WdfWaitLockRelease(Context->PowerLock);

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(out), &outBuf, &outLen);
    if (!NT_SUCCESS(status) || outLen < sizeof(out)) {
        return STATUS_ACCESS_VIOLATION;
    }
    RtlCopyMemory(outBuf, &out, sizeof(out));
    WdfRequestSetInformation(Request, sizeof(out));
    return STATUS_SUCCESS;
}
#endif // TT_DEBUG_INTERFACES

_Use_decl_annotations_
VOID
TtEvtIoDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode
    )
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PTT_DEVICE_CONTEXT context = TtGetDeviceContext(device);
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    UINT32 nr = TtIoctlNr(IoControlCode);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    TraceLoggingWrite(g_TtTraceProvider, "IoctlEntry",
                      TraceLoggingUInt32(nr, "nr"));

    // reset_rwsem parity (DD-9): RESET_DEVICE exclusive, all else shared. The
    // gate check runs under the lock, matching chardev.c:596-624.
    if (nr == 6) {
        TtResetAcquireExclusive(context);
    } else {
        TtResetAcquireShared(context);
    }

    status = TtCheckIoGates(context, fileObject, nr);
    if (!NT_SUCCESS(status)) {
        goto release;
    }

    if (IoControlCode == IOCTL_TENSTORRENT_QUERY_TELEMETRY) {
        status = TtIoctlQueryTelemetry(context, Request);
        goto release;
    }
    if (IoControlCode == IOCTL_TENSTORRENT_FREE_DMA_BUF_EX) {
        status = (fileObject != NULL)
                     ? TtIoctlFreeDmaBufEx(context, fileObject, Request)
                     : STATUS_INVALID_DEVICE_REQUEST;
        goto release;
    }

#ifdef TT_DEBUG_INTERFACES
    if (IoControlCode == IOCTL_TENSTORRENT_DEBUG_READ_TELEMETRY) {
        status = TtIoctlDebugReadTelemetry(context, Request);
        goto release;
    }
    if (IoControlCode == IOCTL_TENSTORRENT_DEBUG_ARC_MSG) {
        status = TtIoctlDebugArcMsg(context, Request);
        goto release;
    }
    if (IoControlCode == IOCTL_TENSTORRENT_DEBUG_GET_AGG_POWER) {
        status = TtIoctlDebugGetAggPower(context, Request);
        goto release;
    }
#endif

    switch (nr) {
    case 0:
        status = TtIoctlGetDeviceInfo(context, Request);
        break;
    case 2:
        status = TtIoctlQueryMappings(context, Request);
        break;
    case 3:
        status = TtIoctlAllocateDmaBuf(context, fileObject, Request);
        break;
    case 5:
        status = TtIoctlGetDriverInfo(Request);
        break;
    case 6:
        status = TtIoctlResetDevice(context, fileObject, Request);
        break;
    case 8: {
        BOOLEAN pended = FALSE;

        status = TtIoctlLockCtl(context, fileObject, Request, &pended);
        if (pended) {
            // Request now owned by the lock wait queue; release the reset lock
            // and return without completing it (a waker completes it later).
            TtResetRelease(context);
            return;
        }
        break;
    }
    case 14:
        status = TtIoctlSetNocCleanup(context, fileObject, Request);
        break;
    case 15:
        status = TtIoctlSetPowerState(context, fileObject, Request);
        break;
    case 11:
        status = TtIoctlAllocateTlb(context, fileObject, Request);
        break;
    case 12:
        status = TtIoctlFreeTlb(context, fileObject, Request);
        break;
    case 13:
        status = TtIoctlConfigureTlb(context, fileObject, Request);
        break;
    case 17:
        // SMC_MSG (tt-kmd 2.11.0, chardev.c ioctl_arc_msg): asynchronous
        // per-handle ARC messaging multiplexed over the firmware queue. Not
        // ported yet; Linux reports -EOPNOTSUPP for a device without a usable
        // queue, which is the closest honest answer until the pump lands
        // (docs/open-questions.md OQ-8). No buffer is read.
        status = STATUS_NOT_SUPPORTED;
        break;
    default:
        // Linux: unhandled commands (incl. GET_HARVESTING and all
        // not-yet-ported nrs) fall through to -EINVAL (chardev.c:694-696).
        // FREE_DMA_BUF (nr 4) gained a handler in tt-kmd 2.10.0; the Windows
        // port still releases DMA buffers at handle close only (OQ-9).
        status = STATUS_INVALID_PARAMETER;
        break;
    }

release:
    TtResetRelease(context);
    TraceLoggingWrite(g_TtTraceProvider, "IoctlExit",
                      TraceLoggingUInt32(nr, "nr"),
                      TraceLoggingNTStatus(status, "status"));
    WdfRequestComplete(Request, status);
}

// MAP/UNMAP/PIN_PAGES/UNPIN_PAGES must run in the calling process context
// (MmMapLockedPagesSpecifyCache / MmProbeAndLockPages act on the current
// process); KMDF queues do not guarantee that, this callback does. All other
// requests are forwarded to the default queue.
_Use_decl_annotations_
VOID
TtEvtIoInCallerContext(
    WDFDEVICE Device,
    WDFREQUEST Request
    )
{
    PTT_DEVICE_CONTEXT context = TtGetDeviceContext(Device);
    WDFFILEOBJECT fileObject = WdfRequestGetFileObject(Request);
    WDF_REQUEST_PARAMETERS params;
    ULONG code;
    UINT32 gateNr;
    NTSTATUS status;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    if (params.Type != WdfRequestTypeDeviceControl || fileObject == NULL) {
        goto forward;
    }
    code = params.Parameters.DeviceIoControl.IoControlCode;

    if (code != IOCTL_TENSTORRENT_MAP && code != IOCTL_TENSTORRENT_UNMAP &&
        code != IOCTL_TENSTORRENT_PIN_PAGES &&
        code != IOCTL_TENSTORRENT_UNPIN_PAGES) {
        goto forward;
    }

    // Pin/unpin are Linux ioctls (full gates incl. needs_hw_init); MAP/UNMAP
    // follow the mmap gate set (no needs_hw_init check, analysis §11). Held
    // shared so a concurrent reset (exclusive) drains these first (DD-9).
    gateNr = (code == IOCTL_TENSTORRENT_PIN_PAGES) ? 7u :
             (code == IOCTL_TENSTORRENT_UNPIN_PAGES) ? 10u : MAXUINT32;
    TtResetAcquireShared(context);
    status = TtCheckIoGates(context, fileObject, gateNr);
    if (NT_SUCCESS(status) &&
        TtGetFileContext(fileObject)->CreatorProcess != PsGetCurrentProcess()) {
        // DD-15: a handle duplicated or inherited into another process may
        // not create or release process-bound state; the address space and
        // the locked-page accounting belong to the opener.
        TraceLoggingWrite(g_TtTraceProvider, "CallerContextWrongProcess",
                          TraceLoggingUInt32(code, "code"));
        status = STATUS_ACCESS_DENIED;
    }
    if (NT_SUCCESS(status)) {
        if (code == IOCTL_TENSTORRENT_MAP) {
            status = TtIoctlMap(context, fileObject, Request);
        } else if (code == IOCTL_TENSTORRENT_UNMAP) {
            status = TtIoctlUnmap(context, fileObject, Request);
        } else if (code == IOCTL_TENSTORRENT_PIN_PAGES) {
            status = TtIoctlPinPages(context, fileObject, Request);
        } else {
            status = TtIoctlUnpinPages(context, fileObject, Request);
        }
    }
    TtResetRelease(context);

    TraceLoggingWrite(g_TtTraceProvider, "CallerContextIoctl",
                      TraceLoggingUInt32(code, "code"),
                      TraceLoggingNTStatus(status, "status"));
    WdfRequestComplete(Request, status);
    return;

forward:
    status = WdfDeviceEnqueueRequest(Device, Request);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
    }
}
