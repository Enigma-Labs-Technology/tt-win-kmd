// Maps to: tt-kmd/chardev.c resource-lock ioctl (ioctl_lock_ctl,
// chardev.c:371-430) + acquire_resource_lock_blocking (chardev.c:323-367).
// 64 device locks; a device "held" bitmap plus a per-handle "held-by-me"
// bitmap. ACQUIRE_BLOCKING that can't win is pended in a manual WDFQUEUE, which
// WDF makes cancellable (CancelIo / handle close -> STATUS_CANCELLED, replacing
// Linux -ERESTARTSYS). Wakers drain the queue and retry each waiter. See DD-10.
#include "ttkmd.h"

#include <stddef.h>
#include "ttkmd_ioctl.h"

// Per-request context: the lock index a pended ACQUIRE_BLOCKING waits on.
typedef struct _TT_LOCK_REQUEST_CONTEXT {
    UINT8 Index;
} TT_LOCK_REQUEST_CONTEXT, *PTT_LOCK_REQUEST_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TT_LOCK_REQUEST_CONTEXT, TtGetLockRequestContext)

_Use_decl_annotations_
NTSTATUS
TtLocksInit(
    PTT_DEVICE_CONTEXT Context
    )
{
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Context->Device;
    status = WdfWaitLockCreate(&attributes, &Context->LockLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Manual queue holds pended ACQUIRE_BLOCKING requests. WDF makes queued
    // requests cancellable automatically.
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Context->Device;
    return WdfIoQueueCreate(Context->Device, &queueConfig, &attributes,
                            &Context->LockWaitQueue);
}

// Try to claim a lock for a handle. Caller holds LockLock. Returns TRUE on win.
static BOOLEAN
TtLockTryAcquire(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ PTT_FILE_CONTEXT FileContext,
    _In_ UINT8 Index
    )
{
    UINT64 bit = 1ull << Index;

    if (Context->ResourceLockHeld & bit) {
        return FALSE;
    }
    // acquire sets global then local (chardev.c:369-370 invariant).
    Context->ResourceLockHeld |= bit;
    FileContext->LocksHeld |= bit;
    return TRUE;
}

// Completes a pended ACQUIRE_BLOCKING request with out.value = 1.
static VOID
TtLockCompleteAcquired(
    _In_ WDFREQUEST Request
    )
{
    struct tenstorrent_lock_ctl_out out;
    PVOID buffer;
    size_t length;
    NTSTATUS status;

    RtlZeroMemory(&out, sizeof(out));
    out.value = 1;

    status = WdfRequestRetrieveOutputBuffer(
        Request, offsetof(struct tenstorrent_lock_ctl, out) + sizeof(out),
        &buffer, &length);
    if (NT_SUCCESS(status)) {
        RtlCopyMemory((PUCHAR)buffer + offsetof(struct tenstorrent_lock_ctl, out),
                      &out, sizeof(out));
        WdfRequestSetInformation(Request,
            offsetof(struct tenstorrent_lock_ctl, out) + sizeof(out));
        WdfRequestComplete(Request, STATUS_SUCCESS);
    } else {
        WdfRequestComplete(Request, STATUS_ACCESS_VIOLATION);
    }
}

// Drain the wait queue and retry each pended waiter. Called under LockLock
// after any state change that could satisfy or invalidate a waiter (RELEASE,
// file cleanup, reset gen-bump, surprise removal). Winners complete value=1,
// stale-gen/detached waiters complete STATUS_DEVICE_REMOVED, losers are
// re-queued. Requests are drained fully into a local batch first (so a
// re-queued loser is never reprocessed within a pass). A batch cap bounds the
// stack array; any overflow simply waits for the next wake (FIFO-fair since
// re-queued losers go to the tail, behind the not-yet-drained overflow).
#define TT_LOCK_WAKE_BATCH 64

_Use_decl_annotations_
VOID
TtLocksWakeWaiters(
    PTT_DEVICE_CONTEXT Context
    )
{
    WDFREQUEST batch[TT_LOCK_WAKE_BATCH];
    ULONG count = 0;
    ULONG i;

    while (count < TT_LOCK_WAKE_BATCH &&
           NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->LockWaitQueue,
                                                    &batch[count]))) {
        count++;
    }

    for (i = 0; i < count; i++) {
        WDFREQUEST request = batch[i];
        PTT_LOCK_REQUEST_CONTEXT reqContext = TtGetLockRequestContext(request);
        WDFFILEOBJECT fileObject = WdfRequestGetFileObject(request);
        PTT_FILE_CONTEXT fileContext =
            (fileObject != NULL) ? TtGetFileContext(fileObject) : NULL;

        if (fileContext == NULL || Context->Detached ||
            fileContext->OpenResetGen != ReadAcquire64(&Context->ResetGen)) {
            WdfRequestComplete(request, STATUS_DEVICE_REMOVED);   // -ENODEV
        } else if (TtLockTryAcquire(Context, fileContext, reqContext->Index)) {
            TtLockCompleteAcquired(request);
        } else {
            NTSTATUS fwd = WdfRequestForwardToIoQueue(request,
                                                      Context->LockWaitQueue);
            if (!NT_SUCCESS(fwd)) {
                WdfRequestComplete(request, fwd);
            }
        }
    }
}

// Releases every lock held by a handle and wakes waiters (chardev.c:877-885).
_Use_decl_annotations_
VOID
TtLocksReleaseAll(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);

    WdfWaitLockAcquire(Context->LockLock, NULL);
    // release clears local then global (chardev.c:369-370 invariant).
    Context->ResourceLockHeld &= ~fileContext->LocksHeld;
    fileContext->LocksHeld = 0;
    TtLocksWakeWaiters(Context);
    WdfWaitLockRelease(Context->LockLock);
}

// LOCK_CTL handler (chardev.c:371-430). *Pended is set TRUE if the request was
// forwarded to the wait queue (caller must NOT complete it).
_Use_decl_annotations_
NTSTATUS
TtIoctlLockCtl(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    WDFREQUEST Request,
    BOOLEAN *Pended
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);
    struct tenstorrent_lock_ctl_in in;
    struct tenstorrent_lock_ctl_out out;
    UINT64 bit;
    NTSTATUS status;

    *Pended = FALSE;

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (in.index >= TENSTORRENT_RESOURCE_LOCK_COUNT) {
        return STATUS_INVALID_PARAMETER;   // -EINVAL (chardev.c:385-386)
    }

    bit = 1ull << in.index;
    RtlZeroMemory(&out, sizeof(out));

    WdfWaitLockAcquire(Context->LockLock, NULL);

    switch (in.flags) {
    case TENSTORRENT_LOCK_CTL_ACQUIRE:
        out.value = TtLockTryAcquire(Context, fileContext, in.index) ? 1 : 0;
        break;

    case TENSTORRENT_LOCK_CTL_RELEASE:
        // Only the holding fd can release (chardev.c:403-410).
        if (fileContext->LocksHeld & bit) {
            fileContext->LocksHeld &= ~bit;
            Context->ResourceLockHeld &= ~bit;
            out.value = 1;
            TtLocksWakeWaiters(Context);
        } else {
            out.value = 0;
        }
        break;

    case TENSTORRENT_LOCK_CTL_TEST:
        // bit0 = held by us, bit1 = held by any (chardev.c:412-416).
        out.value = (UINT8)(((Context->ResourceLockHeld & bit) ? 2 : 0) |
                            ((fileContext->LocksHeld & bit) ? 1 : 0));
        break;

    case TENSTORRENT_LOCK_CTL_ACQUIRE_BLOCKING:
        if (TtLockTryAcquire(Context, fileContext, in.index)) {
            out.value = 1;
        } else {
            // Pend: stash the index and forward to the manual queue. WDF makes
            // it cancellable; a waker retries it.
            PTT_LOCK_REQUEST_CONTEXT reqContext;
            WDF_OBJECT_ATTRIBUTES attributes;

            WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,
                                                    TT_LOCK_REQUEST_CONTEXT);
            status = WdfObjectAllocateContext(Request, &attributes,
                                              (PVOID *)&reqContext);
            if (!NT_SUCCESS(status)) {
                WdfWaitLockRelease(Context->LockLock);
                return status;
            }
            reqContext->Index = in.index;

            status = WdfRequestForwardToIoQueue(Request, Context->LockWaitQueue);
            WdfWaitLockRelease(Context->LockLock);
            if (NT_SUCCESS(status)) {
                *Pended = TRUE;
            }
            return status;
        }
        break;

    default:
        WdfWaitLockRelease(Context->LockLock);
        return STATUS_INVALID_PARAMETER;   // -EINVAL (chardev.c:417-418)
    }

    WdfWaitLockRelease(Context->LockLock);

    return TtCompleteSizedOutBuffer(Request,
                                    offsetof(struct tenstorrent_lock_ctl, out),
                                    &out, sizeof(out), sizeof(out));
}
