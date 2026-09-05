// SPDX-License-Identifier: GPL-2.0-only
// WDF queue substitute; compiled with actual driver wake/claim functions.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define _In_
#define VOID void
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_REMOVED -1
#define NT_SUCCESS(x) ((x) >= 0)
typedef int BOOLEAN;
typedef int NTSTATUS;
typedef uint8_t UINT8;
typedef uint64_t UINT64;
typedef unsigned long ULONG;
typedef struct file { UINT64 LocksHeld; long long OpenResetGen; } *PTT_FILE_CONTEXT, *WDFFILEOBJECT;
typedef struct { UINT8 Index; } TT_LOCK_REQUEST_CONTEXT, *PTT_LOCK_REQUEST_CONTEXT;
typedef struct request { TT_LOCK_REQUEST_CONTEXT ctx; WDFFILEOBJECT file; int completed; } *WDFREQUEST;
typedef struct queue { WDFREQUEST requests[128]; unsigned count; } *WDFQUEUE;
typedef struct device { UINT64 ResourceLockHeld; long long ResetGen; int Detached; WDFQUEUE LockWaitQueue; } *PTT_DEVICE_CONTEXT;
static unsigned completed;
static long long ReadAcquire64(long long *p) { return *p; }
static PTT_LOCK_REQUEST_CONTEXT TtGetLockRequestContext(WDFREQUEST r) { return &r->ctx; }
static WDFFILEOBJECT WdfRequestGetFileObject(WDFREQUEST r) { return r->file; }
static PTT_FILE_CONTEXT TtGetFileContext(WDFFILEOBJECT f) { return f; }
static int WdfIoQueueRetrieveNextRequest(WDFQUEUE q, WDFREQUEST *out) {
 if (!q->count) return -1;
 *out=q->requests[0]; --q->count; memmove(q->requests,q->requests+1,q->count*sizeof(q->requests[0])); return 0;
}
static int WdfRequestForwardToIoQueue(WDFREQUEST r,WDFQUEUE q) { q->requests[q->count++]=r; return 0; }
static void WdfRequestComplete(WDFREQUEST r,int status) { (void)status; r->completed=1; ++completed; }
static void TtLockCompleteAcquired(WDFREQUEST r) { WdfRequestComplete(r,0); }
static int WdfIoQueueGetState(WDFQUEUE q, ULONG *count, void *owned) {
 (void)owned; *count=q->count; return 0;
}
/* DRIVER_FUNCTIONS */

int main(void) {
 struct file file = {0}; struct queue queue = {0}; struct device dev = {0}; struct request requests[65] = {0};
 dev.LockWaitQueue=&queue; dev.ResourceLockHeld=1; /* lock 0 held, lock 1 just released */
 for(unsigned i=0;i<65;++i) { requests[i].file=&file; requests[i].ctx.Index = i==64?1:0; queue.requests[queue.count++]=&requests[i]; }
 TtLocksWakeWaiters(&dev);
 printf("release lock 1 with 64 lock-0 waiters ahead: completed=%u, pending=%u, lock-1 remains free=%d\n",completed,queue.count,!(dev.ResourceLockHeld&2));
 assert(completed==1 && queue.count==64 && (dev.ResourceLockHeld&2));
 dev.Detached=1; completed=0;
 TtLocksWakeWaiters(&dev);
 printf("detach with remaining queued waiters: completed=%u, pending=%u\n",completed,queue.count);
 assert(completed==64 && queue.count==0);
 return 0;
}
