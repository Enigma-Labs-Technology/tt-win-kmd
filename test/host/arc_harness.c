// SPDX-License-Identifier: GPL-2.0-only
// Delayed reply scenario for the actual ARC transaction admission function.
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define VOID void
#define TRUE 1
#define FALSE 0
#define MAXUINT32 UINT32_MAX
#define TraceLoggingWrite(...) ((void)0)
#define KeMemoryBarrier() ((void)0)
typedef int BOOLEAN;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uint64_t ULONGLONG;
#define _In_
#define _Inout_
#define _Inout_opt_
#define _Out_
#define NTSTATUS int
// The real constants and message structure, without WDK dependencies.
#include "blackhole.h"
struct _TT_DEVICE_CONTEXT { void *ArcMsgLock; BOOLEAN ArcExchangeUncertain; };
static unsigned pushes, pops, reads;
static BOOLEAN reply_ready;
static void WdfWaitLockAcquire(void *l,void *t) { (void)l; (void)t; }
static void WdfWaitLockRelease(void *l) { (void)l; }
static ULONGLONG TtBhDeadline(UINT32 ms) { return ms; }
static ULONGLONG KeQueryInterruptTime(void) { return 0; }
static void TtBhPollDelay(void) {}
static UINT32 TtBhNocRead32(struct _TT_DEVICE_CONTEXT *c,UINT32 x,UINT32 y,UINT64 a,UINT32 n) {
 (void)c; (void)x; (void)y; (void)n; ++reads;
 return a==TT_BH_ARC_BOOT_STATUS ? TT_BH_ARC_BOOT_STATUS_READY_FOR_MSG : TT_ARC_CSM_BASE;
}
static BOOLEAN TtBhCsmRangeValid(UINT64 a,UINT64 n) { (void)a; (void)n; return TRUE; }
static BOOLEAN TtBhCsmRead32(struct _TT_DEVICE_CONTEXT *c,UINT64 a,UINT32 *v) {
 (void)c;
 if(a==TT_ARC_CSM_BASE) *v=TT_ARC_CSM_BASE+0x100;
 else if(a==TT_ARC_CSM_BASE+4) *v=8;
 else *v=0;
 return TRUE;
}
static BOOLEAN TtBhCsmWrite32(struct _TT_DEVICE_CONTEXT *c,UINT64 a,UINT32 v) {
 (void)c; (void)a; (void)v; return TRUE;
}
static void TtBhNocWrite32(struct _TT_DEVICE_CONTEXT *c,UINT32 x,UINT32 y,UINT64 a,UINT32 d,UINT32 n) {
 (void)c; (void)x; (void)y; (void)a; (void)d; (void)n;
}
static BOOLEAN TtBhArcMsgPush(struct _TT_DEVICE_CONTEXT *c,PTT_ARC_MSG m,UINT32 b,UINT32 n) {
 (void)c; (void)m; (void)b; (void)n; ++pushes; return TRUE;
}
static BOOLEAN TtBhArcMsgPop(struct _TT_DEVICE_CONTEXT *c,PTT_ARC_MSG m,UINT32 b,UINT32 n) {
 (void)c; (void)b; (void)n; ++pops; m->Header=0; return reply_ready;
}
/* DRIVER_FUNCTIONS */
int main(void) {
 struct _TT_DEVICE_CONTEXT context={0}; TT_ARC_MSG msg={0};
 assert(!TtBhSendArcMessage(&context,&msg)); // A published, response times out
 assert(pushes==1 && pops==1 && context.ArcExchangeUncertain);
 reply_ready=TRUE; reads=0; // A's successful response arrives late
 assert(!TtBhSendArcMessage(&context,&msg)); // B must neither publish nor consume A
 assert(pushes==1 && pops==1 && reads==0);
 struct _TT_DEVICE_CONTEXT recovered={0}; // separate instance after platform recovery
 assert(TtBhSendArcMessage(&recovered,&msg) && !recovered.ArcExchangeUncertain);
}
