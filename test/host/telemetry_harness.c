// SPDX-License-Identifier: GPL-2.0-only
// Scripted device reads for the actual telemetry probe; no hardware access.
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define _In_
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY -1
#define STATUS_NOT_SUPPORTED -2
#define STATUS_DEVICE_DATA_ERROR -3
#define TT_ARC_CSM_BASE 0x10000000u
#define TT_ARC_CSM_SIZE (1u << 19)
#define TT_TELEM_TAG_CACHE_SIZE 128
#define TT_TELEMETRY_MAX_ENTRIES 4096u
#define TT_BH_ARC_X 8
#define TT_BH_ARC_Y 0
#define TT_BH_ARC_TELEMETRY_PTR 1
#define TT_BH_ARC_TELEMETRY_DATA 2
#define MAXUINT32 UINT32_MAX
#define TraceLoggingWrite(...) ((void)0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
typedef int BOOLEAN;
typedef int NTSTATUS;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef struct _TT_DEVICE_CONTEXT {
 UINT64 TelemetryTagCache[128]; BOOLEAN TelemetryValid;
} *PTT_DEVICE_CONTEXT;
static UINT32 base, data, entries, tag;
static unsigned reads;
static BOOLEAN TtBhCsmRangeValid(UINT64 Address, UINT64 Length);
static UINT32 TtBhNocRead32(PTT_DEVICE_CONTEXT c, UINT32 x, UINT32 y, UINT64 a, UINT32 n) {
 (void)c; (void)x; (void)y; (void)n; ++reads;
 if(a==TT_BH_ARC_TELEMETRY_PTR) return base;
 if(a==TT_BH_ARC_TELEMETRY_DATA) return data;
 if(a==base) return 0x10000;
 if(a==(UINT64)base+4) return entries;
 return tag;
}
static BOOLEAN TtBhCsmRead32(PTT_DEVICE_CONTEXT c, UINT64 a, UINT32 *v) {
 if(!TtBhCsmRangeValid(a,4)) return FALSE;
 *v=TtBhNocRead32(c,0,0,a,0); return TRUE;
}
/* DRIVER_FUNCTIONS */
int main(void) {
 struct _TT_DEVICE_CONTEXT c={0};
 assert(!TtBhCsmRangeValid(UINT64_MAX,4));
 assert(!TtBhCsmRangeValid(TT_ARC_CSM_BASE-4,4));
 assert(!TtBhCsmRangeValid(TT_ARC_CSM_BASE+1,4));
 assert(!TtBhCsmRangeValid(TT_ARC_CSM_BASE,UINT64_MAX));
 assert(TtBhCsmRangeValid(TT_ARC_CSM_BASE+TT_ARC_CSM_SIZE-4,4));
 base=TT_ARC_CSM_BASE; data=base+0x100; entries=1; tag=(2u<<16)|7;
 assert(TtBhTelemetryProbe(&c)==0 && c.TelemetryValid && c.TelemetryTagCache[7]==data+8);
 entries=UINT32_MAX; reads=0;
 assert(TtBhTelemetryProbe(&c)==STATUS_DEVICE_DATA_ERROR && !c.TelemetryValid && reads==4);
 assert(c.TelemetryTagCache[7]==0);
 base=TT_ARC_CSM_BASE+TT_ARC_CSM_SIZE-4; reads=0;
 assert(TtBhTelemetryProbe(&c)==STATUS_DEVICE_NOT_READY && reads==2);
 base=TT_ARC_CSM_BASE; data=TT_ARC_CSM_BASE+TT_ARC_CSM_SIZE-4; entries=1; tag=(2u<<16)|7;
 assert(TtBhTelemetryProbe(&c)==STATUS_DEVICE_DATA_ERROR && !c.TelemetryValid && c.TelemetryTagCache[7]==0);
}
