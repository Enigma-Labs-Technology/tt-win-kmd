// Maps to: tt-kmd/blackhole.c (kernel-TLB NOC access, CSM accessors,
// send_arc_message, telemetry_probe/read_telemetry_tag) + tt-kmd/msgqueue.c
// (arc_msg_push/arc_msg_pop) — the M2 subset.
//
// Ordering note (analysis §09.6): Linux relies on x86 MMIO accessor ordering
// with no explicit barriers. Here KeMemoryBarrier() separates entry-writes →
// WPTR publish → doorbell, per the porting note, so the sequence stays correct
// on weaker-ordered targets too.
#include "ttkmd.h"
#include "ttkmd_ioctl.h"
#include "blackhole.h"

// --- 2 MiB TLB window register (blackhole.c:112-137) ---------------------
//
// 96-bit register, 12 bytes, three 32-bit words, little-endian bit layout:
//   address:43 | x_end:6 | y_end:6 | x_start:6 | y_start:6 | noc:2 |
//   multicast:1 | ordering:2 | linked:1 | use_static_vc:1 | stream_header:1 |
//   static_vc:3 | reserved:18
// GCC packed bitfields are not portable to MSVC; compose words explicitly.

static VOID
TtBhSetBits(
    _Inout_updates_(3) UINT32 Words[3],
    _In_ ULONG BitOffset,
    _In_ ULONG BitLength,
    _In_ UINT64 Value
    )
{
    ULONG i;

    for (i = 0; i < BitLength; i++) {
        if ((Value >> i) & 1) {
            Words[(BitOffset + i) / 32] |= 1u << ((BitOffset + i) % 32);
        }
    }
}

// Programs the driver-owned kernel TLB window (window 201) at the 2 MiB
// granule containing (x, y, addr) and returns the kernel VA for addr.
// Caller holds KernelTlbLock. Maps to bh_configure_kernel_tlb
// (blackhole.c:229-243) + blackhole_configure_tlb_2M (165-198).
static volatile UCHAR *
TtBhConfigureKernelTlb(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ UINT32 X,
    _In_ UINT32 Y,
    _In_ UINT64 Addr,
    _In_ UINT32 Noc
    )
{
    UINT32 words[3] = { 0, 0, 0 };
    UINT64 offset = Addr & (TT_BH_KERNEL_TLB_LEN - 1);
    UINT64 base = Addr & ~((UINT64)TT_BH_KERNEL_TLB_LEN - 1);
    volatile UCHAR *regs =
        Context->TlbRegs + (TT_BH_KERNEL_TLB_INDEX * TT_BH_TLB_REG_SIZE);

    TtBhSetBits(words, 0, 43, base >> 21);   // address (2M-aligned, shifted)
    TtBhSetBits(words, 43, 6, X);            // x_end
    TtBhSetBits(words, 49, 6, Y);            // y_end
    // x_start/y_start = 0 (no multicast)
    TtBhSetBits(words, 67, 2, Noc);          // noc
    TtBhSetBits(words, 70, 2, 1);            // ordering = strict

    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 0), words[0]);
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 4), words[1]);
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 8), words[2]);
    // Window 201 >= TLB_STRIDED_COUNT (32): no strided register to clear
    // (blackhole.c:192-196 conditional never fires for the kernel window).

    return Context->KernelTlb + offset;
}

// blackhole_configure_tlb_2M / _4G parity (blackhole.c:112-198): programs a
// user window's 96-bit config register. The uAPI `static_vc` byte maps to the
// hardware `use_static_vc` bit; the 3-bit hw static_vc field and stream_header
// are never programmed (upstream parity). Strided config is cleared for the
// first 32 2M windows (blackhole.c:192-196).
_Use_decl_annotations_
NTSTATUS
TtBhConfigureUserTlb(
    struct _TT_DEVICE_CONTEXT *Context,
    UINT32 Id,
    const struct tenstorrent_noc_tlb_config *Config
    )
{
    UINT32 words[3] = { 0, 0, 0 };
    volatile UCHAR *regs = Context->TlbRegs + (Id * TT_BH_TLB_REG_SIZE);
    BOOLEAN is2M = Id < TT_TLB_2M_COUNT;
    UINT64 windowMask = is2M ? (TT_BH_KERNEL_TLB_LEN - 1) : ((1ull << 32) - 1);

    if (Config->addr & windowMask) {
        return STATUS_INVALID_PARAMETER;   // -EINVAL: unaligned window base
    }

    if (is2M) {
        TtBhSetBits(words, 0, 43, Config->addr >> 21);
        TtBhSetBits(words, 43, 6, Config->x_end);
        TtBhSetBits(words, 49, 6, Config->y_end);
        TtBhSetBits(words, 55, 6, Config->x_start);
        TtBhSetBits(words, 61, 6, Config->y_start);
        TtBhSetBits(words, 67, 2, Config->noc);
        TtBhSetBits(words, 69, 1, Config->mcast);
        TtBhSetBits(words, 70, 2, Config->ordering);
        TtBhSetBits(words, 72, 1, Config->linked);
        TtBhSetBits(words, 73, 1, Config->static_vc);   // use_static_vc
    } else {
        TtBhSetBits(words, 0, 32, Config->addr >> 32);
        TtBhSetBits(words, 32, 6, Config->x_end);
        TtBhSetBits(words, 38, 6, Config->y_end);
        TtBhSetBits(words, 44, 6, Config->x_start);
        TtBhSetBits(words, 50, 6, Config->y_start);
        TtBhSetBits(words, 56, 2, Config->noc);
        TtBhSetBits(words, 58, 1, Config->mcast);
        TtBhSetBits(words, 59, 2, Config->ordering);
        TtBhSetBits(words, 61, 1, Config->linked);
        TtBhSetBits(words, 62, 1, Config->static_vc);   // use_static_vc
    }

    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 0), words[0]);
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 4), words[1]);
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 8), words[2]);

    if (Id < 32) {   // TLB_STRIDED_COUNT
        volatile UCHAR *strided = Context->TlbRegs +
            (TT_TLB_2M_COUNT + TT_TLB_4G_MAX) * TT_BH_TLB_REG_SIZE + (Id * 4);

        WRITE_REGISTER_ULONG((volatile ULONG *)strided, 0);
    }

    return STATUS_SUCCESS;
}

// blackhole_configure_outbound_atu parity (blackhole.c:755-788): programs one
// outbound iATU region in BAR2 (IATU_BASE 0x1000, stride 0x100).
_Use_decl_annotations_
NTSTATUS
TtBhConfigureOutboundAtu(
    struct _TT_DEVICE_CONTEXT *Context,
    UINT32 Region,
    UINT64 Base,
    UINT64 Limit,
    UINT64 Target
    )
{
    volatile UCHAR *regs;
    UINT64 size = Limit - Base + 1;
    UINT32 ctrl1 = 1u << 13;                       // INCREASE_REGION_SIZE
    UINT32 ctrl2 = (Limit == 0) ? 0 : (1u << 31);  // REGION_EN

    if (Limit != 0 && size > (1ull << 40)) {
        return STATUS_INVALID_PARAMETER;   // iATU max region 1T
    }
    if (Region >= TT_IATU_REGIONS) {
        return STATUS_INVALID_PARAMETER;
    }

    regs = Context->Bar2Mapping + 0x1000 + (Region * 0x100);
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 0x08), (UINT32)Base);
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 0x0C), (UINT32)(Base >> 32));
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 0x14), (UINT32)Target);
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 0x18), (UINT32)(Target >> 32));
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 0x10), (UINT32)Limit);
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 0x20), (UINT32)(Limit >> 32));
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 0x00), ctrl1);
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 0x04), ctrl2);
    WRITE_REGISTER_ULONG((volatile ULONG *)(regs + 0x1C), 0);

    TraceLoggingWrite(g_TtTraceProvider, "IatuConfigured",
                      TraceLoggingUInt32(Region, "region"),
                      TraceLoggingUInt64(Base, "base"),
                      TraceLoggingUInt64(Limit, "limit"),
                      TraceLoggingUInt64(Target, "target"));
    return STATUS_SUCCESS;
}

// Maps to noc_read32/noc_write32 (blackhole.c:245-268): each 32-bit access
// reprograms the kernel window under the lock, exactly like Linux.
static UINT32
TtBhNocRead32(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ UINT32 X,
    _In_ UINT32 Y,
    _In_ UINT64 Addr,
    _In_ UINT32 Noc
    )
{
    volatile UCHAR *window;
    UINT32 value;

    WdfWaitLockAcquire(Context->KernelTlbLock, NULL);
    window = TtBhConfigureKernelTlb(Context, X, Y, Addr, Noc);
    value = READ_REGISTER_ULONG((volatile ULONG *)window);
    WdfWaitLockRelease(Context->KernelTlbLock);
    return value;
}

static VOID
TtBhNocWrite32(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ UINT32 X,
    _In_ UINT32 Y,
    _In_ UINT64 Addr,
    _In_ UINT32 Data,
    _In_ UINT32 Noc
    )
{
    volatile UCHAR *window;

    WdfWaitLockAcquire(Context->KernelTlbLock, NULL);
    window = TtBhConfigureKernelTlb(Context, X, Y, Addr, Noc);
    WRITE_REGISTER_ULONG((volatile ULONG *)window, Data);
    WdfWaitLockRelease(Context->KernelTlbLock);
}

// Maps to is_range_within_csm (telemetry.h:75-78) + csm_read32/csm_write32
// (blackhole.c:270-286). Returns FALSE on range violation (-EINVAL parity).
static BOOLEAN
TtBhCsmRead32(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ UINT64 Addr,
    _Out_ UINT32 *Value
    )
{
    if (Addr < TT_ARC_CSM_BASE ||
        Addr > (UINT64)TT_ARC_CSM_BASE + TT_ARC_CSM_SIZE - sizeof(UINT32)) {
        *Value = 0;
        return FALSE;
    }
    *Value = TtBhNocRead32(Context, TT_BH_ARC_X, TT_BH_ARC_Y, Addr, 0);
    return TRUE;
}

static BOOLEAN
TtBhCsmWrite32(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ UINT64 Addr,
    _In_ UINT32 Value
    )
{
    if (Addr < TT_ARC_CSM_BASE ||
        Addr > (UINT64)TT_ARC_CSM_BASE + TT_ARC_CSM_SIZE - sizeof(UINT32)) {
        return FALSE;
    }
    TtBhNocWrite32(Context, TT_BH_ARC_X, TT_BH_ARC_Y, Addr, Value, 0);
    return TRUE;
}

// --- message queue engine (tt-kmd/msgqueue.c port) ------------------------

static ULONGLONG
TtBhDeadline(
    _In_ UINT32 Milliseconds
    )
{
    return KeQueryInterruptTime() + (ULONGLONG)Milliseconds * 10000;
}

static VOID
TtBhPollDelay(
    VOID
    )
{
    // usleep_range(100, 200) parity (msgqueue.c:52,112)
    LARGE_INTEGER interval;

    interval.QuadPart = -1000;   // 100 us relative
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

// arc_msg_push (msgqueue.c:12-70): enqueue one request. All-ones pointer
// reads abort early (device gone / NOC hung); 1000 ms timeout otherwise.
static BOOLEAN
TtBhArcMsgPush(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ const TT_ARC_MSG *Msg,
    _In_ UINT32 QueueBase,
    _In_ UINT32 NumEntries
    )
{
    UINT32 requestBase = QueueBase + TT_ARC_MSG_QUEUE_HEADER_SIZE;
    UINT32 wptr, rptr, slot;
    ULONGLONG deadline;
    ULONG i;

    if (!TtBhCsmRead32(Context, QueueBase + TT_ARC_MSG_QUEUE_REQ_WPTR, &wptr)) {
        return FALSE;
    }
    if (wptr == MAXUINT32) {
        TraceLoggingWrite(g_TtTraceProvider, "ArcQueueGoneWptr");
        return FALSE;
    }

    deadline = TtBhDeadline(TT_ARC_MSG_TIMEOUT_MS);
    for (;;) {
        if (!TtBhCsmRead32(Context, QueueBase + TT_ARC_MSG_QUEUE_REQ_RPTR, &rptr)) {
            return FALSE;
        }
        if (rptr == MAXUINT32) {
            TraceLoggingWrite(g_TtTraceProvider, "ArcQueueGoneRptr");
            return FALSE;
        }
        if (((wptr - rptr) % (2 * NumEntries)) < NumEntries) {
            break;   // space available
        }
        if (KeQueryInterruptTime() > deadline) {
            TraceLoggingWrite(g_TtTraceProvider, "ArcQueuePushTimeout");
            return FALSE;
        }
        TtBhPollDelay();
    }

    slot = wptr % NumEntries;
    for (i = 0; i < 8; i++) {
        UINT32 addr = requestBase + slot * (UINT32)sizeof(TT_ARC_MSG) +
                      (UINT32)(i * sizeof(UINT32));
        UINT32 value = (i == 0) ? Msg->Header : Msg->Payload[i - 1];

        if (!TtBhCsmWrite32(Context, addr, value)) {
            return FALSE;
        }
    }

    KeMemoryBarrier();   // entry words visible before the WPTR publish

    wptr = (wptr + 1) % (2 * NumEntries);
    return TtBhCsmWrite32(Context, QueueBase + TT_ARC_MSG_QUEUE_REQ_WPTR, wptr);
}

// arc_msg_pop (msgqueue.c:72-132): dequeue one response.
static BOOLEAN
TtBhArcMsgPop(
    _In_ PTT_DEVICE_CONTEXT Context,
    _Out_ PTT_ARC_MSG Msg,
    _In_ UINT32 QueueBase,
    _In_ UINT32 NumEntries
    )
{
    UINT32 responseBase = QueueBase + TT_ARC_MSG_QUEUE_HEADER_SIZE +
                          NumEntries * (UINT32)sizeof(TT_ARC_MSG);
    UINT32 rptr, wptr, slot, base;
    ULONGLONG deadline;
    ULONG i;

    RtlZeroMemory(Msg, sizeof(*Msg));

    if (!TtBhCsmRead32(Context, QueueBase + TT_ARC_MSG_QUEUE_RES_RPTR, &rptr)) {
        return FALSE;
    }
    if (rptr == MAXUINT32) {
        TraceLoggingWrite(g_TtTraceProvider, "ArcQueueGoneRptr");
        return FALSE;
    }

    deadline = TtBhDeadline(TT_ARC_MSG_TIMEOUT_MS);
    for (;;) {
        if (!TtBhCsmRead32(Context, QueueBase + TT_ARC_MSG_QUEUE_RES_WPTR, &wptr)) {
            return FALSE;
        }
        if (wptr == MAXUINT32) {
            TraceLoggingWrite(g_TtTraceProvider, "ArcQueueGoneWptr");
            return FALSE;
        }
        if (((wptr - rptr) % (2 * NumEntries)) > 0) {
            break;   // response available
        }
        if (KeQueryInterruptTime() > deadline) {
            TraceLoggingWrite(g_TtTraceProvider, "ArcQueuePopTimeout");
            return FALSE;
        }
        TtBhPollDelay();
    }

    KeMemoryBarrier();   // RES_WPTR observed before the entry loads

    slot = rptr % NumEntries;
    base = responseBase + slot * (UINT32)sizeof(TT_ARC_MSG);
    if (!TtBhCsmRead32(Context, base, &Msg->Header)) {
        return FALSE;
    }
    for (i = 0; i < 7; i++) {
        if (!TtBhCsmRead32(Context, base + (UINT32)((i + 1) * sizeof(UINT32)),
                           &Msg->Payload[i])) {
            return FALSE;
        }
    }

    rptr = (rptr + 1) % (2 * NumEntries);
    return TtBhCsmWrite32(Context, QueueBase + TT_ARC_MSG_QUEUE_RES_RPTR, rptr);
}

// send_arc_message (blackhole.c:500-540): ready-gate, QCB discovery,
// push -> doorbell -> pop as one synchronous transaction. Msg is overwritten
// with the response; TRUE only when the response header is 0.
//
// Deviation (documented): Linux takes no lock here and leaves serialization
// to callers; ArcMsgLock makes the transaction atomic against concurrent
// senders, a safe superset with no ABI-visible effect.
_Use_decl_annotations_
BOOLEAN
TtBhSendArcMessage(
    struct _TT_DEVICE_CONTEXT *Context,
    PTT_ARC_MSG Msg
    )
{
    UINT32 bootStatus = 0;
    UINT32 queueCtrlAddr, queueBase, queueInfo, numEntries;
    ULONGLONG deadline;
    BOOLEAN ok = FALSE;

    WdfWaitLockAcquire(Context->ArcMsgLock, NULL);

    deadline = TtBhDeadline(TT_ARC_MSG_READY_MS);
    for (;;) {
        bootStatus = TtBhNocRead32(Context, TT_BH_ARC_X, TT_BH_ARC_Y,
                                   TT_BH_ARC_BOOT_STATUS, 0);
        if (bootStatus == MAXUINT32) {
            goto out;   // NOC is hung (blackhole.c:511-512)
        }
        if (bootStatus & TT_BH_ARC_BOOT_STATUS_READY_FOR_MSG) {
            break;
        }
        if (KeQueryInterruptTime() > deadline) {
            break;
        }
        TtBhPollDelay();
    }
    if (!(bootStatus & TT_BH_ARC_BOOT_STATUS_READY_FOR_MSG)) {
        goto out;
    }

    queueCtrlAddr = TtBhNocRead32(Context, TT_BH_ARC_X, TT_BH_ARC_Y,
                                  TT_BH_ARC_MSG_QCB_PTR, 0);
    if (!TtBhCsmRead32(Context, queueCtrlAddr + 0, &queueBase) ||
        !TtBhCsmRead32(Context, queueCtrlAddr + 4, &queueInfo)) {
        goto out;
    }
    numEntries = queueInfo & 0xFF;
    if (numEntries == 0) {
        goto out;
    }

    if (!TtBhArcMsgPush(Context, Msg, queueBase, numEntries)) {
        goto out;
    }

    KeMemoryBarrier();   // WPTR visible before the doorbell
    TtBhNocWrite32(Context, TT_BH_ARC_X, TT_BH_ARC_Y, TT_BH_ARC_MSI_FIFO, 0, 0);

    if (!TtBhArcMsgPop(Context, Msg, queueBase, numEntries)) {
        goto out;
    }

    ok = (Msg->Header == 0);   // blackhole.c:539

out:
    WdfWaitLockRelease(Context->ArcMsgLock);
    TraceLoggingWrite(g_TtTraceProvider, "ArcMessage",
                      TraceLoggingBoolean(ok, "success"),
                      TraceLoggingUInt32(Msg->Header, "responseHeader"));
    return ok;
}

// telemetry_probe (blackhole.c:455-498): discover the tag table via the
// scratch pointers and cache per-tag CSM value addresses.
_Use_decl_annotations_
NTSTATUS
TtBhTelemetryProbe(
    struct _TT_DEVICE_CONTEXT *Context
    )
{
    UINT32 baseAddr, dataAddr, tagsAddr;
    UINT32 version, majorVer;
    UINT32 numEntries;
    UINT32 i;

    RtlZeroMemory(Context->TelemetryTagCache, sizeof(Context->TelemetryTagCache));
    Context->TelemetryValid = FALSE;

    baseAddr = TtBhNocRead32(Context, TT_BH_ARC_X, TT_BH_ARC_Y,
                             TT_BH_ARC_TELEMETRY_PTR, 0);
    dataAddr = TtBhNocRead32(Context, TT_BH_ARC_X, TT_BH_ARC_Y,
                             TT_BH_ARC_TELEMETRY_DATA, 0);
    tagsAddr = baseAddr + 8;

    if (baseAddr < TT_ARC_CSM_BASE ||
        baseAddr >= TT_ARC_CSM_BASE + TT_ARC_CSM_SIZE ||
        dataAddr < TT_ARC_CSM_BASE ||
        dataAddr >= TT_ARC_CSM_BASE + TT_ARC_CSM_SIZE) {
        TraceLoggingWrite(g_TtTraceProvider, "TelemetryNotAvailable");
        return STATUS_DEVICE_NOT_READY;   // -ENODEV parity
    }

    version = TtBhNocRead32(Context, TT_BH_ARC_X, TT_BH_ARC_Y, baseAddr, 0);
    majorVer = (version >> 16) & 0xFF;
    if (majorVer > 1) {
        TraceLoggingWrite(g_TtTraceProvider, "TelemetryVersionUnsupported",
                          TraceLoggingUInt32(version, "version"));
        return STATUS_NOT_SUPPORTED;   // -ENOTSUPP parity
    }

    numEntries = TtBhNocRead32(Context, TT_BH_ARC_X, TT_BH_ARC_Y,
                               baseAddr + 4, 0);

    for (i = 0; i < numEntries; i++) {
        UINT32 tagEntry = TtBhNocRead32(Context, TT_BH_ARC_X, TT_BH_ARC_Y,
                                        tagsAddr + (i * 4), 0);
        UINT16 tagId = tagEntry & 0xFFFF;
        UINT16 offset = (tagEntry >> 16) & 0xFFFF;

        if (tagId < TT_TELEM_TAG_CACHE_SIZE) {
            Context->TelemetryTagCache[tagId] =
                (UINT64)dataAddr + ((UINT64)offset * 4);
        }
    }

    Context->TelemetryValid = TRUE;
    TraceLoggingWrite(g_TtTraceProvider, "TelemetryProbed",
                      TraceLoggingUInt32(numEntries, "entries"));
    return STATUS_SUCCESS;
}

// blackhole_read_telemetry_tag (blackhole.c:440-453)
_Use_decl_annotations_
NTSTATUS
TtBhReadTelemetryTag(
    struct _TT_DEVICE_CONTEXT *Context,
    UINT16 TagId,
    UINT32 *Value
    )
{
    UINT64 addr;

    *Value = 0;
    if (TagId >= TT_TELEM_TAG_CACHE_SIZE) {
        return STATUS_INVALID_PARAMETER;   // -EINVAL parity
    }
    addr = Context->TelemetryTagCache[TagId];
    if (addr == 0) {
        return STATUS_NOT_FOUND;           // -ENODATA parity
    }
    if (!TtBhCsmRead32(Context, addr, Value)) {
        return STATUS_ACCESS_VIOLATION;    // csm range error (-EINVAL)
    }
    return STATUS_SUCCESS;
}
