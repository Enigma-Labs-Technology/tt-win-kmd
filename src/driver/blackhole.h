// Maps to: tt-kmd/blackhole.h + the ARC/telemetry constants of blackhole.c
// (lines 56-74) and msgqueue.h.
#pragma once

// ARC tile NOC coordinates (blackhole.c:57-58)
#define TT_BH_ARC_X 8
#define TT_BH_ARC_Y 0

// RESET_SCRATCH(N) = 0x80030400 + 4N (blackhole.c:59)
#define TT_BH_RESET_SCRATCH(n) (0x80030400u + ((n) * 4u))
#define TT_BH_ARC_BOOT_STATUS TT_BH_RESET_SCRATCH(2)   // bit 0 = ready for msgs
#define TT_BH_ARC_BOOT_STATUS_READY_FOR_MSG 0x1u
#define TT_BH_ARC_MSG_QCB_PTR TT_BH_RESET_SCRATCH(11)  // queue control block ptr
#define TT_BH_ARC_TELEMETRY_DATA TT_BH_RESET_SCRATCH(12)
#define TT_BH_ARC_TELEMETRY_PTR TT_BH_RESET_SCRATCH(13)
#define TT_BH_ARC_MSI_FIFO 0x800B0000u                 // write 0 = doorbell

// ARC CSM address gate (tt-kmd/telemetry.h:73-78)
#define TT_ARC_CSM_BASE 0x10000000u
#define TT_ARC_CSM_SIZE (1u << 19)

// Message queue protocol (tt-kmd/msgqueue.h; analysis §09)
#define TT_ARC_MSG_QUEUE_HEADER_SIZE 32u
#define TT_ARC_MSG_QUEUE_REQ_WPTR 0x00u   // driver writes
#define TT_ARC_MSG_QUEUE_RES_RPTR 0x04u   // driver writes
#define TT_ARC_MSG_QUEUE_REQ_RPTR 0x10u   // firmware writes
#define TT_ARC_MSG_QUEUE_RES_WPTR 0x14u   // firmware writes
#define TT_ARC_MSG_TIMEOUT_MS 1000u       // msgqueue.h ARC_MSG_TIMEOUT_MS
#define TT_ARC_MSG_READY_MS 500u          // blackhole.c ARC_MSG_READY_MS

// Message types the driver sends (blackhole.c:66-72)
#define TT_ARC_MSG_TYPE_POWER_SETTING 0x21u
#define TT_ARC_MSG_TYPE_TRIGGER_RESET 0x56u
#define TT_ARC_MSG_TYPE_TEST 0x90u
#define TT_ARC_MSG_TYPE_ASIC_STATE0 0xA0u
#define TT_ARC_MSG_TYPE_ASIC_STATE3 0xA3u
#define TT_ARC_MSG_TYPE_SET_WDT_TIMEOUT 0xC1u

// Telemetry (tt-kmd/telemetry.h)
#define TT_TELEM_TAG_CACHE_SIZE 128
#define TT_TELEMETRY_TIMER_HEARTBEAT 32

// 32-byte fixed message (tt-kmd/msgqueue.h:11-14)
typedef struct _TT_ARC_MSG {
    UINT32 Header;
    UINT32 Payload[7];
} TT_ARC_MSG, *PTT_ARC_MSG;

struct _TT_DEVICE_CONTEXT;

NTSTATUS TtBhTelemetryProbe(_Inout_ struct _TT_DEVICE_CONTEXT *Context);
NTSTATUS TtBhReadTelemetryTag(_In_ struct _TT_DEVICE_CONTEXT *Context,
                              _In_ UINT16 TagId, _Out_ UINT32 *Value);
BOOLEAN TtBhSendArcMessage(_In_ struct _TT_DEVICE_CONTEXT *Context,
                           _Inout_ PTT_ARC_MSG Msg);
