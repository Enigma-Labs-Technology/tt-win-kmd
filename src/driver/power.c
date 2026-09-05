// SPDX-FileCopyrightText: 2026 tt-win-kmd contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Maps to: tt-kmd/chardev.c power aggregation (ioctl_set_power_state,
// chardev.c:562-589; tenstorrent_set_aggregated_power_state_locked,
// chardev.c:478-545). Multi-client: each handle stores a contribution; the
// aggregate (flags OR'd with "unspecified defaults ON", settings by max) is
// sent to firmware via an ARC POWER_SETTING message. See DD-10.
#include "ttkmd.h"

#include <stddef.h>
#include "ttkmd_ioctl.h"
#include "blackhole.h"

#define TT_POWER_FLAG_ALL 0x7FFFu   // bits 0-14; bit 15 reserved (chardev.c:29)

// Legacy open default (chardev.c:821-823): all flags on except MAX_AI_CLK,
// validity = TT_POWER_VALIDITY(15, 0), and it contributes immediately.
_Use_decl_annotations_
VOID
TtPowerFileDefault(
    WDFFILEOBJECT FileObject
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);

    fileContext->PowerContributes = TRUE;
    fileContext->PowerValidity = (UINT8)TT_POWER_VALIDITY(15, 0);
    fileContext->PowerFlags = (UINT16)(TT_POWER_FLAG_ALL & ~TT_POWER_FLAG_MAX_AI_CLK);
    RtlZeroMemory(fileContext->PowerSettings, sizeof(fileContext->PowerSettings));
}

// Aggregate across all live handles and send to firmware. Caller must NOT hold
// PowerLock; this takes it (and the FileListLock) itself.
_Use_decl_annotations_
VOID
TtPowerAggregate(
    PTT_DEVICE_CONTEXT Context
    )
{
    UINT16 aggFlags = 0;
    UINT16 aggSettings[14];
    UINT8 maxSettingsCount = 0;
    LONG64 gen = ReadAcquire64(&Context->ResetGen);
    PLIST_ENTRY entry;
    TT_ARC_MSG msg;
    ULONG i;

    RtlZeroMemory(aggSettings, sizeof(aggSettings));

    WdfWaitLockAcquire(Context->PowerLock, NULL);
    WdfWaitLockAcquire(Context->FileListLock, NULL);

    for (entry = Context->FileList.Flink; entry != &Context->FileList;
         entry = entry->Flink) {
        PTT_FILE_CONTEXT fc =
            CONTAINING_RECORD(entry, TT_FILE_CONTEXT, DeviceLink);
        UINT8 flagsCount, settingsCount;
        UINT16 unspecifiedMask, effectiveFlags;

        WdfWaitLockAcquire(fc->Lock, NULL);
        // Skip stale-generation handles (chardev.c:493-495) and non-contributors.
        if (!fc->PowerContributes || fc->OpenResetGen != gen) {
            WdfWaitLockRelease(fc->Lock);
            continue;
        }

        flagsCount = (fc->PowerValidity >> 0) & 0xF;
        settingsCount = (fc->PowerValidity >> 4) & 0xF;
        if (settingsCount > maxSettingsCount) {
            maxSettingsCount = settingsCount;
        }

        // Flags past flags_count default to ON (chardev.c:509): old clients
        // cannot disable features they do not know about.
        unspecifiedMask = (UINT16)(~((1u << flagsCount) - 1) & TT_POWER_FLAG_ALL);
        effectiveFlags = (UINT16)(fc->PowerFlags | unspecifiedMask);
        aggFlags |= effectiveFlags;

        if (settingsCount > ARRAYSIZE(aggSettings)) {
            settingsCount = ARRAYSIZE(aggSettings);
        }
        for (i = 0; i < settingsCount; i++) {
            if (fc->PowerSettings[i] > aggSettings[i]) {
                aggSettings[i] = fc->PowerSettings[i];
            }
        }
        WdfWaitLockRelease(fc->Lock);
    }

    WdfWaitLockRelease(Context->FileListLock);

    Context->PowerAggValid = FALSE;
    Context->PowerAggFlags = aggFlags;
    RtlCopyMemory(Context->PowerAggSettings, aggSettings, sizeof(aggSettings));

    TraceLoggingWrite(g_TtTraceProvider, "PowerAggregate",
                      TraceLoggingUInt16(aggFlags, "flags"),
                      TraceLoggingUInt8(maxSettingsCount, "maxSettings"));

    // Send to firmware (blackhole_set_power_state -> ARC POWER_SETTING 0x21).
    // On-wire layout per blackhole.c:802-804 + chardev.c:531: the HEADER packs
    // type | validity<<8 | flags<<16, and the payload carries ALL 14 settings,
    // two u16 per dword. ttsim consumed any layout as a no-op, which hid an
    // earlier mis-pack (validity/flags in payload[0], 12 settings dropped) —
    // real firmware parses these fields.
    if (Context->HardwareReady && Context->IsBlackhole && !Context->Detached) {
        RtlZeroMemory(&msg, sizeof(msg));
        msg.Header = TT_ARC_MSG_TYPE_POWER_SETTING |
                     ((UINT32)TT_POWER_VALIDITY(15, maxSettingsCount) << 8) |
                     ((UINT32)aggFlags << 16);
        for (i = 0; i < ARRAYSIZE(aggSettings) / 2; i++) {
            msg.Payload[i] = (UINT32)aggSettings[2 * i] |
                             ((UINT32)aggSettings[2 * i + 1] << 16);
        }
        Context->PowerAggValid = TtBhSendArcMessage(Context, &msg);
    } else if (Context->HardwareReady && !Context->IsBlackhole) {
        Context->PowerAggValid = TRUE;
    }
    // Serialize snapshot publication through firmware completion.
    WdfWaitLockRelease(Context->PowerLock);
}

// ioctl_set_power_state (chardev.c:562-589). argsz protocol (input-only).
_Use_decl_annotations_
NTSTATUS
TtIoctlSetPowerState(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    WDFREQUEST Request
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);
    struct tenstorrent_power_state in;
    NTSTATUS status;

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Validation (chardev.c:567-579).
    if (in.argsz != sizeof(in)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (in.flags != 0 || in.reserved0 != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (in.validity > TT_POWER_VALIDITY(15, 14)) {
        return STATUS_INVALID_PARAMETER;
    }

    // Store this handle's contribution (chardev.c:584-586).
    WdfWaitLockAcquire(fileContext->Lock, NULL);
    fileContext->PowerContributes = TRUE;
    fileContext->PowerValidity = in.validity;
    fileContext->PowerFlags = in.power_flags;
    RtlCopyMemory(fileContext->PowerSettings, in.power_settings,
                  sizeof(fileContext->PowerSettings));
    WdfWaitLockRelease(fileContext->Lock);

    // Re-aggregate and send (chardev.c:588). Input-only ioctl: no output.
    TtPowerAggregate(Context);
    WdfRequestSetInformation(Request, 0);
    return STATUS_SUCCESS;
}

// ioctl_set_noc_cleanup (chardev.c:432-475). argsz protocol, input-only. Stores
// a NOC write to perform at handle close (device-side cleanup on abnormal exit).
_Use_decl_annotations_
NTSTATUS
TtIoctlSetNocCleanup(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    WDFREQUEST Request
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);
    struct tenstorrent_set_noc_cleanup in;
    NTSTATUS status;

    if (!Context->IsBlackhole) {
        return STATUS_NOT_SUPPORTED;   // no noc_write32 -> -EOPNOTSUPP
    }

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    // Validation order (chardev.c:448-469).
    if (in.argsz != sizeof(in) || in.flags != 0 || in.enabled > 1 ||
        (in.addr & 0x3) != 0 || in.noc > 1 || in.x > 64 || in.y > 64) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(fileContext->Lock, NULL);
    fileContext->NocCleanupEnabled = (in.enabled != 0);
    fileContext->NocCleanupX = in.x;
    fileContext->NocCleanupY = in.y;
    fileContext->NocCleanupNoc = in.noc;
    fileContext->NocCleanupAddr = in.addr;
    fileContext->NocCleanupData = in.data;
    WdfWaitLockRelease(fileContext->Lock);

    WdfRequestSetInformation(Request, 0);
    return STATUS_SUCCESS;
}
