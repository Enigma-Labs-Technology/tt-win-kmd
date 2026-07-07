// Maps to: tt-kmd/memory.c (mmap offset namespace, DMA buffers, iATU
// allocator, pin/unpin) + tt-kmd/tlb.c (TLB pool) — the M3 subset per DD-8.
//
// Process-context requirement: MAP/UNMAP/PIN_PAGES/UNPIN_PAGES are serviced
// from EvtIoInCallerContext (ioctl.c) so MmMapLockedPagesSpecifyCache and
// MmProbeAndLockPages act on the calling process. Handle cleanup runs in the
// closing process (IRP_MJ_CLEANUP), where user unmaps are legal.
#include "ttkmd.h"

#include <stddef.h>
#include "ttkmd_ioctl.h"
#include "blackhole.h"

// ---------------------------------------------------------------------------
// File-context memory state
// ---------------------------------------------------------------------------

_Use_decl_annotations_
NTSTATUS
TtFileContextInitMemory(
    WDFFILEOBJECT FileObject
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);
    WDF_OBJECT_ATTRIBUTES attributes;

    InitializeListHead(&fileContext->Mappings);
    InitializeListHead(&fileContext->Pinnings);
    RtlZeroMemory(fileContext->DmaBufs, sizeof(fileContext->DmaBufs));

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = FileObject;
    return WdfWaitLockCreate(&attributes, &fileContext->Lock);
}

// ---------------------------------------------------------------------------
// TLB pool (tt-kmd/tlb.c + memory.c:893-999)
// ---------------------------------------------------------------------------

// describe_tlb parity (blackhole_describe_tlb): id -> BAR, byte offset, size.
static NTSTATUS
TtTlbDescribe(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ UINT32 Id,
    _Out_ UINT32 *Bar,
    _Out_ UINT64 *BarOffset,
    _Out_ UINT64 *Size
    )
{
    if (Id < TT_TLB_2M_COUNT) {
        *Bar = 0;
        *BarOffset = (UINT64)Id * TT_BH_KERNEL_TLB_LEN;
        *Size = TT_BH_KERNEL_TLB_LEN;
        return STATUS_SUCCESS;
    }
    if (Id < TT_TLB_2M_COUNT + Context->Tlb4gCount) {
        *Bar = 4;
        *BarOffset = (UINT64)(Id - TT_TLB_2M_COUNT) << 32;
        *Size = 1ull << 32;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}

_Use_decl_annotations_
NTSTATUS
TtIoctlAllocateTlb(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    WDFREQUEST Request
    )
{
    struct tenstorrent_allocate_tlb_in in;
    struct tenstorrent_allocate_tlb_out out;
    UINT32 poolStart, poolCount, id;
    UINT64 barOffset, winSize, encoded;
    UINT32 bar;
    BOOLEAN found = FALSE;
    NTSTATUS status;

    if (!Context->IsBlackhole) {
        return STATUS_INVALID_PARAMETER;   // no tlb_kinds -> -EINVAL
    }

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Exact-size pool selection, no rounding (tlb.c:20-33).
    if (in.size == TT_BH_KERNEL_TLB_LEN) {
        poolStart = 0;
        poolCount = TT_TLB_2M_COUNT;
    } else if (in.size == (1ull << 32)) {
        poolStart = TT_TLB_2M_COUNT;
        poolCount = Context->Tlb4gCount;
    } else {
        return STATUS_INVALID_PARAMETER;   // -EINVAL
    }

    WdfWaitLockAcquire(Context->TlbLock, NULL);
    for (id = poolStart; id < poolStart + poolCount; id++) {
        if (!Context->TlbUsed[id]) {
            Context->TlbUsed[id] = TRUE;
            Context->TlbOwner[id] = FileObject;
            found = TRUE;
            break;
        }
    }
    WdfWaitLockRelease(Context->TlbLock);

    if (!found) {
        return STATUS_INSUFFICIENT_RESOURCES;   // -ENOMEM
    }

    (VOID)TtTlbDescribe(Context, id, &bar, &barOffset, &winSize);

    // mmap-offset encoding (memory.c:924-934): BAR0 windows encode their BAR0
    // byte offset; BAR4 windows are re-based at BAR0_SIZE = 512 MiB.
    encoded = barOffset;
    if (bar == 4) {
        encoded += 1ull << 29;
    }

    RtlZeroMemory(&out, sizeof(out));
    out.id = id;
    out.mmap_offset_uc = TT_MMAP_OFFSET_TLB_UC + encoded;
    out.mmap_offset_wc = TT_MMAP_OFFSET_TLB_WC + encoded;

    // Fixed-size out (protocol 3d)
    status = TtCompleteSizedOutBuffer(Request,
                                      offsetof(struct tenstorrent_allocate_tlb, out),
                                      &out, sizeof(out), sizeof(out));
    if (!NT_SUCCESS(status)) {
        WdfWaitLockAcquire(Context->TlbLock, NULL);
        Context->TlbUsed[id] = FALSE;
        Context->TlbOwner[id] = NULL;
        WdfWaitLockRelease(Context->TlbLock);
    }
    return status;
}

_Use_decl_annotations_
NTSTATUS
TtIoctlFreeTlb(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    WDFREQUEST Request
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);
    struct tenstorrent_free_tlb_in in;
    PLIST_ENTRY entry;
    NTSTATUS status;

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (in.id >= TENSTORRENT_MAX_INBOUND_TLBS) {
        return STATUS_INVALID_PARAMETER;   // -EINVAL (memory.c:955-956)
    }

    WdfWaitLockAcquire(fileContext->Lock, NULL);
    WdfWaitLockAcquire(Context->TlbLock, NULL);

    if (in.id >= TT_TLB_TOTAL || !Context->TlbUsed[in.id] ||
        Context->TlbOwner[in.id] != FileObject) {
        status = STATUS_ACCESS_DENIED;     // -EPERM (memory.c:960-963)
        goto unlock;
    }

    // Live user mapping of this window -> -EBUSY (memory.c:965-974).
    for (entry = fileContext->Mappings.Flink; entry != &fileContext->Mappings;
         entry = entry->Flink) {
        PTT_USER_MAPPING mapping = CONTAINING_RECORD(entry, TT_USER_MAPPING, Entry);

        if (mapping->TlbId == (LONG)in.id) {
            status = STATUS_DEVICE_BUSY;
            goto unlock;
        }
    }

    // Hardware registers deliberately not cleared (upstream parity, DD-8).
    Context->TlbUsed[in.id] = FALSE;
    Context->TlbOwner[in.id] = NULL;
    status = STATUS_SUCCESS;

unlock:
    WdfWaitLockRelease(Context->TlbLock);
    WdfWaitLockRelease(fileContext->Lock);
    return status;
}

_Use_decl_annotations_
NTSTATUS
TtIoctlConfigureTlb(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    WDFREQUEST Request
    )
{
    struct tenstorrent_configure_tlb_in in;
    struct tenstorrent_configure_tlb_out out;
    NTSTATUS status;

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (in.id >= TENSTORRENT_MAX_INBOUND_TLBS) {
        return STATUS_INVALID_PARAMETER;   // -EINVAL (memory.c:992-993)
    }

    WdfWaitLockAcquire(Context->TlbLock, NULL);
    if (in.id >= TT_TLB_TOTAL || !Context->TlbUsed[in.id] ||
        Context->TlbOwner[in.id] != FileObject) {
        WdfWaitLockRelease(Context->TlbLock);
        return STATUS_ACCESS_DENIED;       // -EPERM (memory.c:995-996)
    }
    WdfWaitLockRelease(Context->TlbLock);

    status = TtBhConfigureUserTlb(Context, in.id, &in.config);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&out, sizeof(out));
    return TtCompleteSizedOutBuffer(Request,
                                    offsetof(struct tenstorrent_configure_tlb, out),
                                    &out, sizeof(out), sizeof(out));
}

// ---------------------------------------------------------------------------
// Outbound iATU allocator (memory.c:39-200, 276-297)
// ---------------------------------------------------------------------------

// First-fit over [0, TT_BH_NOC_DMA_LIMIT], regions sorted by base. Returns the
// claimed region index or -1. Caller holds IatuLock.
static LONG
TtIatuFindAndClaim(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject,
    _In_ BOOLEAN TopDown,
    _In_ UINT64 Size,
    _Out_ UINT64 *Base
    )
{
    struct { UINT64 Base; UINT64 Limit; } used[TT_IATU_REGIONS];
    ULONG usedCount = 0;
    ULONG i, j;
    UINT64 gapStart;
    LONG slot = -1;

    // Collect and insertion-sort in-use regions by base (memory.c:39-65).
    for (i = 0; i < TT_IATU_REGIONS; i++) {
        if (Context->Iatu[i].Used) {
            UINT64 b = Context->Iatu[i].Base;
            UINT64 l = Context->Iatu[i].Limit;

            for (j = usedCount; j > 0 && used[j - 1].Base > b; j--) {
                used[j] = used[j - 1];
            }
            used[j].Base = b;
            used[j].Limit = l;
            usedCount++;
        } else if (slot < 0) {
            slot = (LONG)i;
        }
    }
    if (slot < 0) {
        return -1;   // -ENOSPC (memory.c:154-155)
    }

    if (!TopDown) {
        // Bottom-up first fit (memory.c:101-133)
        gapStart = 0;
        for (i = 0; i < usedCount; i++) {
            if (used[i].Base > gapStart && used[i].Base - gapStart >= Size) {
                break;
            }
            gapStart = used[i].Limit + 1;
        }
        if (gapStart + Size - 1 > TT_BH_NOC_DMA_LIMIT) {
            return -1;   // -ENOMEM
        }
        *Base = gapStart;
    } else {
        // Top-down first fit (memory.c:67-99)
        UINT64 gapEnd = TT_BH_NOC_DMA_LIMIT;
        BOOLEAN placed = FALSE;

        for (i = usedCount; i > 0; i--) {
            if (gapEnd >= used[i - 1].Limit &&
                gapEnd - used[i - 1].Limit >= Size) {
                placed = TRUE;
                break;
            }
            if (used[i - 1].Base == 0) {
                return -1;
            }
            gapEnd = used[i - 1].Base - 1;
        }
        if (!placed && gapEnd + 1 < Size) {
            return -1;
        }
        *Base = gapEnd - Size + 1;
    }

    Context->Iatu[slot].Used = TRUE;
    Context->Iatu[slot].Owner = FileObject;
    Context->Iatu[slot].Base = *Base;
    Context->Iatu[slot].Limit = *Base + Size - 1;
    return slot;
}

// setup_noc_dma parity (memory.c:172-200): claim a region, program the
// hardware, report noc_pcie_offset + base.
static NTSTATUS
TtSetupNocDma(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject,
    _In_ BOOLEAN TopDown,
    _In_ UINT64 Size,
    _In_ UINT64 Target,
    _Out_ UINT64 *NocAddress,
    _Out_ LONG *Region
    )
{
    UINT64 base = 0;
    LONG slot;
    NTSTATUS status;

    *NocAddress = 0;
    *Region = -1;
    if (Size == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(Context->IatuLock, NULL);
    slot = TtIatuFindAndClaim(Context, FileObject, TopDown, Size, &base);
    if (slot < 0) {
        WdfWaitLockRelease(Context->IatuLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = TtBhConfigureOutboundAtu(Context, (UINT32)slot, base,
                                      base + Size - 1, Target);
    if (!NT_SUCCESS(status)) {
        Context->Iatu[slot].Used = FALSE;
        Context->Iatu[slot].Owner = NULL;
        WdfWaitLockRelease(Context->IatuLock);
        return status;
    }
    WdfWaitLockRelease(Context->IatuLock);

    *NocAddress = TT_BH_NOC_PCIE_OFFSET + base;
    *Region = slot;
    return STATUS_SUCCESS;
}

// teardown_outbound_iatu parity (memory.c:276-297). Skips the hardware write
// when detached (device gone).
static VOID
TtTeardownNocDma(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ LONG Region
    )
{
    if (Region < 0 || Region >= (LONG)TT_IATU_REGIONS) {
        return;
    }

    WdfWaitLockAcquire(Context->IatuLock, NULL);
    if (!Context->Detached) {
        (VOID)TtBhConfigureOutboundAtu(Context, (UINT32)Region, 0, 0, 0);
    }
    Context->Iatu[Region].Used = FALSE;
    Context->Iatu[Region].Owner = NULL;
    Context->Iatu[Region].Base = 0;
    Context->Iatu[Region].Limit = 0;
    Context->Iatu[Region].Target = 0;
    WdfWaitLockRelease(Context->IatuLock);
}

// ---------------------------------------------------------------------------
// Coherent DMA buffers (memory.c:425-523)
// ---------------------------------------------------------------------------

_Use_decl_annotations_
NTSTATUS
TtIoctlAllocateDmaBuf(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    WDFREQUEST Request
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);
    struct tenstorrent_allocate_dma_buf_in in;
    struct tenstorrent_allocate_dma_buf_out out;
    PTT_DMABUF dmabuf = NULL;
    WDFCOMMONBUFFER buffer = NULL;
    PHYSICAL_ADDRESS logical;
    UINT64 nocAddress = 0;
    LONG iatuRegion = -1;
    NTSTATUS status;

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Validation order per memory.c:439-458.
    if (Context->DmaEnabler == NULL) {
        return STATUS_INVALID_PARAMETER;   // !dma_capable -> -EINVAL
    }
    if (in.buf_index >= TENSTORRENT_MAX_DMA_BUFS) {
        return STATUS_INVALID_PARAMETER;
    }
    if (in.requested_size == 0 || (in.requested_size % PAGE_SIZE) != 0 ||
        in.requested_size > TT_MAX_DMA_BUF_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(fileContext->Lock, NULL);

    if (fileContext->DmaBufs[in.buf_index] != NULL) {
        status = STATUS_INVALID_PARAMETER; // duplicate index -> -EINVAL
        goto unlock;
    }

    status = WdfCommonBufferCreate(Context->DmaEnabler, in.requested_size,
                                   WDF_NO_OBJECT_ATTRIBUTES, &buffer);
    if (!NT_SUCCESS(status)) {
        status = STATUS_INSUFFICIENT_RESOURCES;   // -ENOMEM
        goto unlock;
    }
    logical = WdfCommonBufferGetAlignedLogicalAddress(buffer);
    RtlZeroMemory(WdfCommonBufferGetAlignedVirtualAddress(buffer),
                  in.requested_size);

    // dma_set_coherent_mask(58) parity (enumerate.c:331, dma_address_bits):
    // the Blackhole DMA engine cannot generate addresses above 2^58, and the
    // WDF enabler profile is 64-bit — enforce the reach here rather than
    // silently handing out an unreachable iATU target.
    if ((UINT64)logical.QuadPart + in.requested_size - 1 > TT_BH_NOC_DMA_LIMIT) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }

    if (in.flags & TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA) {
        // DMA-buffer NOC mappings are always top-down (memory.c:477).
        status = TtSetupNocDma(Context, FileObject, TRUE, in.requested_size,
                               (UINT64)logical.QuadPart, &nocAddress,
                               &iatuRegion);
        if (!NT_SUCCESS(status)) {
            goto fail;
        }
    }

    dmabuf = (PTT_DMABUF)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*dmabuf),
                                         TT_TAG_DMABUF);
    if (dmabuf == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    dmabuf->Buffer = buffer;
    dmabuf->Size = in.requested_size;
    dmabuf->IatuRegion = iatuRegion;

    RtlZeroMemory(&out, sizeof(out));
    out.physical_address = (UINT64)logical.QuadPart;
    out.mapping_offset = TT_MMAP_OFFSET_DMA_BUF +
                         (UINT64)in.buf_index * TT_MMAP_SIZE_DMA_BUF;
    out.size = in.requested_size;
    out.noc_address = nocAddress;

    status = TtCompleteSizedOutBuffer(Request,
                                      offsetof(struct tenstorrent_allocate_dma_buf, out),
                                      &out, sizeof(out), sizeof(out));
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    fileContext->DmaBufs[in.buf_index] = dmabuf;
    WdfWaitLockRelease(fileContext->Lock);

    TraceLoggingWrite(g_TtTraceProvider, "DmaBufAllocated",
                      TraceLoggingUInt32(in.buf_index, "index"),
                      TraceLoggingUInt32(in.requested_size, "size"),
                      TraceLoggingUInt64(out.physical_address, "bus"),
                      TraceLoggingUInt64(nocAddress, "noc"));
    return STATUS_SUCCESS;

fail:
    if (dmabuf != NULL) {
        ExFreePoolWithTag(dmabuf, TT_TAG_DMABUF);
    }
    // DD-8: tear down the aperture BEFORE freeing the memory it targets.
    TtTeardownNocDma(Context, iatuRegion);
    if (buffer != NULL) {
        WdfObjectDelete(buffer);
    }
unlock:
    WdfWaitLockRelease(fileContext->Lock);
    return status;
}

// ---------------------------------------------------------------------------
// MAP / UNMAP (DD-8; decode parity with tenstorrent_mmap, memory.c:1585-1636)
// ---------------------------------------------------------------------------

// Resolves an mmap-offset token to (CPU physical range, cache type, tlb id).
// The physical address is what the user MDL is built from, so for DMA buffers
// it is the CPU physical of the buffer (MmGetPhysicalAddress), not the bus
// (logical) address handed to the device.
static NTSTATUS
TtResolveMapTarget(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ PTT_FILE_CONTEXT FileContext,
    _In_ UINT64 MmapOffset,
    _In_ UINT64 Length,
    _Out_ PHYSICAL_ADDRESS *Physical,
    _Out_ MEMORY_CACHING_TYPE *CacheType,
    _Out_ LONG *TlbId
    )
{
    static const ULONG regionToBar[6] = { 0, 0, 2, 2, 4, 4 };

    *TlbId = -1;

    if (MmapOffset >= TT_MMAP_OFFSET_DMA_BUF) {
        // DMA buffer slot (memory.c:1344-1368): fixed 4 GiB stride by index.
        UINT64 rel = MmapOffset - TT_MMAP_OFFSET_DMA_BUF;
        UINT64 index = rel / TT_MMAP_SIZE_DMA_BUF;
        PTT_DMABUF dmabuf;

        if ((rel % TT_MMAP_SIZE_DMA_BUF) != 0 ||
            index >= TENSTORRENT_MAX_DMA_BUFS) {
            return STATUS_INVALID_PARAMETER;
        }
        dmabuf = FileContext->DmaBufs[index];
        if (dmabuf == NULL || Length > dmabuf->Size) {
            return STATUS_INVALID_PARAMETER;
        }
        *Physical = MmGetPhysicalAddress(
            WdfCommonBufferGetAlignedVirtualAddress(dmabuf->Buffer));
        *CacheType = MmCached;
        return STATUS_SUCCESS;
    }

    if (MmapOffset < 6ull * TT_MMAP_RESOURCE_SIZE) {
        // BAR regions 0..5 (memory.c:1596-1618)
        ULONG region = (ULONG)(MmapOffset / TT_MMAP_RESOURCE_SIZE);
        UINT64 offset = MmapOffset % TT_MMAP_RESOURCE_SIZE;
        ULONG bar = regionToBar[region];

        if (Context->BarLength[bar] == 0 || offset >= Context->BarLength[bar] ||
            Length > Context->BarLength[bar] - offset) {
            return STATUS_INVALID_PARAMETER;
        }
        Physical->QuadPart = Context->BarBase[bar].QuadPart + (LONGLONG)offset;
        *CacheType = (region & 1) ? MmWriteCombined : MmNonCached;
        return STATUS_SUCCESS;
    }

    if (MmapOffset < 8ull * TT_MMAP_RESOURCE_SIZE) {
        // TLB window (memory.c:1494-1583, 1620-1626): encoded = BAR0 byte
        // offset, BAR4 windows re-based at 512 MiB. Ownership enforced.
        BOOLEAN wc = MmapOffset >= TT_MMAP_OFFSET_TLB_WC;
        UINT64 encoded = MmapOffset -
                         (wc ? TT_MMAP_OFFSET_TLB_WC : TT_MMAP_OFFSET_TLB_UC);
        UINT32 bar;
        UINT64 barOffset;
        UINT32 id;

        if (encoded < (1ull << 29)) {
            bar = 0;
            barOffset = encoded;
            id = (UINT32)(encoded / TT_BH_KERNEL_TLB_LEN);
            if ((encoded % TT_BH_KERNEL_TLB_LEN) != 0 ||
                Length > TT_BH_KERNEL_TLB_LEN) {
                return STATUS_INVALID_PARAMETER;
            }
        } else {
            bar = 4;
            barOffset = encoded - (1ull << 29);
            id = TT_TLB_2M_COUNT + (UINT32)(barOffset >> 32);
            if ((barOffset % (1ull << 32)) != 0 || Length > (1ull << 32)) {
                return STATUS_INVALID_PARAMETER;
            }
        }

        if (id >= TT_TLB_TOTAL || Context->BarLength[bar] == 0 ||
            barOffset + Length > Context->BarLength[bar]) {
            return STATUS_INVALID_PARAMETER;
        }

        WdfWaitLockAcquire(Context->TlbLock, NULL);
        if (!Context->TlbUsed[id] ||
            Context->TlbOwner[id] != WdfObjectContextGetObject(FileContext)) {
            WdfWaitLockRelease(Context->TlbLock);
            return STATUS_ACCESS_DENIED;   // window not owned by this handle
        }
        WdfWaitLockRelease(Context->TlbLock);

        Physical->QuadPart = Context->BarBase[bar].QuadPart + (LONGLONG)barOffset;
        *CacheType = wc ? MmWriteCombined : MmNonCached;
        *TlbId = (LONG)id;
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

// Builds an MDL describing physical pages [Physical, Physical+Length) — device
// BAR pages or a common buffer's contiguous pages. This is the correct way to
// expose device/IO physical memory to user mode; MmBuildMdlForNonPagedPool
// only works for nonpaged-pool VAs and produces wrong PFNs for either case.
static PMDL
TtBuildPhysicalMdl(
    _In_ PHYSICAL_ADDRESS Physical,
    _In_ SIZE_T Length
    )
{
    PMDL mdl = IoAllocateMdl(NULL, (ULONG)Length, FALSE, FALSE, NULL);
    PPFN_NUMBER pfns;
    PFN_NUMBER basePfn;
    ULONG pageCount;
    ULONG i;

    if (mdl == NULL) {
        return NULL;
    }
    pageCount = (ULONG)BYTES_TO_PAGES(Length);
    basePfn = (PFN_NUMBER)(Physical.QuadPart >> PAGE_SHIFT);
    pfns = MmGetMdlPfnArray(mdl);
    for (i = 0; i < pageCount; i++) {
        pfns[i] = basePfn + i;
    }
    // Marking a hand-built physical MDL "locked" is the sanctioned idiom for
    // exposing device/IO memory to user mode (WDK mapmem sample); the pages are
    // fixed device/contiguous frames, not pageable RAM. CA flags the opaque
    // write generically.
#pragma warning(suppress: 28145)
    mdl->MdlFlags |= MDL_PAGES_LOCKED;
    return mdl;
}

// Destroys a mapping in the owner's process context (UNMAP and handle cleanup
// both run there). Not used by the reset zap, which handles cross-process
// unmapping itself.
static VOID
TtDestroyUserMapping(
    _In_ PTT_USER_MAPPING Mapping
    )
{
    if (Mapping->UserVa != NULL && Mapping->Mdl != NULL) {
        MmUnmapLockedPages(Mapping->UserVa, Mapping->Mdl);
    }
    if (Mapping->Mdl != NULL) {
        IoFreeMdl(Mapping->Mdl);
    }
    if (Mapping->Process != NULL) {
        ObDereferenceObject(Mapping->Process);
    }
    ExFreePoolWithTag(Mapping, TT_TAG_MAPPING);
}

_Use_decl_annotations_
NTSTATUS
TtIoctlMap(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    WDFREQUEST Request
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);
    struct tenstorrent_map_in in;
    struct tenstorrent_map_out out;
    PHYSICAL_ADDRESS physical;
    MEMORY_CACHING_TYPE cacheType;
    LONG tlbId;
    PTT_USER_MAPPING mapping = NULL;
    NTSTATUS status;

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (in.length == 0 || (in.length % PAGE_SIZE) != 0 ||
        (in.mmap_offset % PAGE_SIZE) != 0 || in.length > MAXULONG) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(fileContext->Lock, NULL);

    status = TtResolveMapTarget(Context, fileContext, in.mmap_offset,
                                in.length, &physical, &cacheType, &tlbId);
    if (!NT_SUCCESS(status)) {
        goto unlock;
    }

    mapping = (PTT_USER_MAPPING)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                sizeof(*mapping),
                                                TT_TAG_MAPPING);
    if (mapping == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto unlock;
    }
    mapping->TlbId = tlbId;
    mapping->Length = (SIZE_T)in.length;
    // DMA-buffer maps are host RAM and are not zapped on reset (Linux parity).
    mapping->IsDmaBuf = (in.mmap_offset >= TT_MMAP_OFFSET_DMA_BUF);
    // Creator process, referenced so a cross-process reset zap can attach to
    // its address space to unmap (DD-9).
    mapping->Process = PsGetCurrentProcess();
    ObReferenceObject(mapping->Process);

    mapping->Mdl = TtBuildPhysicalMdl(physical, (SIZE_T)in.length);
    if (mapping->Mdl == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }

    __try {
        mapping->UserVa = MmMapLockedPagesSpecifyCache(
            mapping->Mdl, UserMode, cacheType, NULL, FALSE,
            NormalPagePriority | MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mapping->UserVa = NULL;
    }
    if (mapping->UserVa == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }

    RtlZeroMemory(&out, sizeof(out));
    out.user_va = (UINT64)(ULONG_PTR)mapping->UserVa;
    status = TtCompleteSizedOutBuffer(Request,
                                      offsetof(struct tenstorrent_map, out),
                                      &out, sizeof(out), sizeof(out));
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    InsertTailList(&fileContext->Mappings, &mapping->Entry);
    WdfWaitLockRelease(fileContext->Lock);

    TraceLoggingWrite(g_TtTraceProvider, "UserMapped",
                      TraceLoggingUInt64(in.mmap_offset, "token"),
                      TraceLoggingUInt64(in.length, "length"),
                      TraceLoggingUInt64(out.user_va, "userVa"));
    return STATUS_SUCCESS;

fail:
    TtDestroyUserMapping(mapping);
unlock:
    WdfWaitLockRelease(fileContext->Lock);
    return status;
}

_Use_decl_annotations_
NTSTATUS
TtIoctlUnmap(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    WDFREQUEST Request
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);
    struct tenstorrent_unmap_in in;
    PLIST_ENTRY entry;
    PTT_USER_MAPPING found = NULL;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Context);

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (in.reserved != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(fileContext->Lock, NULL);
    for (entry = fileContext->Mappings.Flink; entry != &fileContext->Mappings;
         entry = entry->Flink) {
        PTT_USER_MAPPING mapping = CONTAINING_RECORD(entry, TT_USER_MAPPING, Entry);

        if ((UINT64)(ULONG_PTR)mapping->UserVa == in.user_va) {
            RemoveEntryList(&mapping->Entry);
            found = mapping;
            break;
        }
    }
    WdfWaitLockRelease(fileContext->Lock);

    if (found == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    TtDestroyUserMapping(found);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// PIN_PAGES / UNPIN_PAGES (memory.c:544-780, DD-8: direct/no-IOMMU path only)
// ---------------------------------------------------------------------------

_Use_decl_annotations_
NTSTATUS
TtIoctlPinPages(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    WDFREQUEST Request
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);
    struct tenstorrent_pin_pages_in in;
    struct tenstorrent_pin_pages_out_extended out;
    PTT_PINNING pinning = NULL;
    PPFN_NUMBER pfns;
    ULONG pageCount;
    ULONG i;
    UINT64 nocAddress = 0;
    LONG iatuRegion = -1;
    PLIST_ENTRY entry;
    BOOLEAN locked = FALSE;
    NTSTATUS status;
    static const UINT32 validFlags =
        TENSTORRENT_PIN_PAGES_CONTIGUOUS | TENSTORRENT_PIN_PAGES_NOC_DMA |
        TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN | TENSTORRENT_PIN_PAGES_READ_ONLY;

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Validation order per memory.c:569-605.
    if (in.flags & ~validFlags) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((in.virtual_address % PAGE_SIZE) != 0 || (in.size % PAGE_SIZE) != 0 ||
        in.size == 0 || in.size > MAXULONG) {
        return STATUS_INVALID_PARAMETER;
    }
    if (in.flags & TENSTORRENT_PIN_PAGES_READ_ONLY) {
        // Enforceable only via IOMMU translation (-EOPNOTSUPP parity, DD-8).
        return STATUS_NOT_SUPPORTED;
    }

    // DD-8: this is the Linux no-IOMMU direct path (memory.c:685-705) — it
    // returns raw physical addresses as device addresses. In a translated
    // (non-identity) DMA domain those are not bus addresses; the device
    // would fault through the IOMMU or silently corrupt whatever the value
    // aliases. Refuse honestly, per the documented DD-8 contract. The domain
    // was probed at PrepareHardware (DmaIdentityProbe).
    if (!Context->DmaIdentityKnown || !Context->DmaIdentityMapped) {
        return STATUS_NOT_SUPPORTED;
    }

    WdfWaitLockAcquire(fileContext->Lock, NULL);

    // Duplicate (VA, size) pin -> -EEXIST (memory.c:594-605).
    for (entry = fileContext->Pinnings.Flink; entry != &fileContext->Pinnings;
         entry = entry->Flink) {
        PTT_PINNING p = CONTAINING_RECORD(entry, TT_PINNING, Entry);

        if (p->VirtualAddress == in.virtual_address && p->Size == in.size) {
            status = STATUS_OBJECT_NAME_COLLISION;
            goto unlock;
        }
    }

    pinning = (PTT_PINNING)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                           sizeof(*pinning), TT_TAG_PINNING);
    if (pinning == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto unlock;
    }
    pinning->VirtualAddress = in.virtual_address;
    pinning->Size = in.size;
    pinning->IatuRegion = -1;

    pinning->Mdl = IoAllocateMdl((PVOID)(ULONG_PTR)in.virtual_address,
                                 (ULONG)in.size, FALSE, FALSE, NULL);
    if (pinning->Mdl == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }

    // pin_user_pages(FOLL_WRITE | FOLL_LONGTERM) analogue.
    __try {
        MmProbeAndLockPages(pinning->Mdl, UserMode, IoWriteAccess);
        locked = TRUE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = STATUS_ACCESS_VIOLATION;   // -EFAULT-class failure
        goto fail;
    }

    // Direct-path contiguity requirement (memory.c:685-694).
    pfns = MmGetMdlPfnArray(pinning->Mdl);
    pageCount = (ULONG)(in.size / PAGE_SIZE);
    for (i = 1; i < pageCount; i++) {
        if (pfns[i] != pfns[i - 1] + 1) {
            status = STATUS_INVALID_PARAMETER;   // -EINVAL
            goto fail;
        }
    }

    RtlZeroMemory(&out, sizeof(out));
    out.physical_address = (UINT64)pfns[0] * PAGE_SIZE;

    if (in.flags & (TENSTORRENT_PIN_PAGES_NOC_DMA |
                    TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN)) {
        BOOLEAN topDown = (in.flags & TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN) != 0;

        status = TtSetupNocDma(Context, FileObject, topDown, in.size,
                               out.physical_address, &nocAddress, &iatuRegion);
        if (!NT_SUCCESS(status)) {
            goto fail;
        }
        pinning->IatuRegion = iatuRegion;
        out.noc_address = nocAddress;
    }

    // output_size_bytes protocol (3a) with the extended out struct.
    status = TtCompleteSizedOutBuffer(Request,
                                      offsetof(struct tenstorrent_pin_pages, out),
                                      &out, sizeof(out), in.output_size_bytes);
    if (!NT_SUCCESS(status)) {
        goto fail;
    }

    InsertTailList(&fileContext->Pinnings, &pinning->Entry);
    WdfWaitLockRelease(fileContext->Lock);

    TraceLoggingWrite(g_TtTraceProvider, "PagesPinned",
                      TraceLoggingUInt64(in.virtual_address, "va"),
                      TraceLoggingUInt64(in.size, "size"),
                      TraceLoggingUInt64(out.physical_address, "physical"));
    return STATUS_SUCCESS;

fail:
    TtTeardownNocDma(Context, iatuRegion);
    if (pinning->Mdl != NULL) {
        if (locked) {
            MmUnlockPages(pinning->Mdl);
        }
        IoFreeMdl(pinning->Mdl);
    }
    ExFreePoolWithTag(pinning, TT_TAG_PINNING);
unlock:
    WdfWaitLockRelease(fileContext->Lock);
    return status;
}

static VOID
TtDestroyPinning(
    _In_ PTT_DEVICE_CONTEXT Context,
    _In_ PTT_PINNING Pinning
    )
{
    TtTeardownNocDma(Context, Pinning->IatuRegion);
    MmUnlockPages(Pinning->Mdl);   // dirties write-locked pages (Linux parity)
    IoFreeMdl(Pinning->Mdl);
    ExFreePoolWithTag(Pinning, TT_TAG_PINNING);
}

_Use_decl_annotations_
NTSTATUS
TtIoctlUnpinPages(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject,
    WDFREQUEST Request
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);
    struct tenstorrent_unpin_pages_in in;
    PLIST_ENTRY entry;
    PTT_PINNING found = NULL;
    NTSTATUS status;

    status = TtCopyInBuffer(Request, &in, sizeof(in));
    if (!NT_SUCCESS(status)) {
        return status;
    }
    // memory.c:750-756: reserved must be 0; size must be nonzero pages.
    if (in.reserved != 0 || in.size == 0 || (in.size / PAGE_SIZE) == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    WdfWaitLockAcquire(fileContext->Lock, NULL);
    for (entry = fileContext->Pinnings.Flink; entry != &fileContext->Pinnings;
         entry = entry->Flink) {
        PTT_PINNING p = CONTAINING_RECORD(entry, TT_PINNING, Entry);

        if (p->VirtualAddress == in.virtual_address) {
            if (p->Size != in.size) {
                // VA match with wrong size -> -EINVAL (partial unsupported)
                break;
            }
            RemoveEntryList(&p->Entry);
            found = p;
            break;
        }
    }
    WdfWaitLockRelease(fileContext->Lock);

    if (found == NULL) {
        return STATUS_INVALID_PARAMETER;   // -EINVAL
    }
    TtDestroyPinning(Context, found);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Handle cleanup (tenstorrent_memory_cleanup parity, memory.c:1638-1666,
// with the DD-8 iATU-before-free ordering fix)
// ---------------------------------------------------------------------------

_Use_decl_annotations_
VOID
TtMemoryFileCleanup(
    PTT_DEVICE_CONTEXT Context,
    WDFFILEOBJECT FileObject
    )
{
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);
    ULONG i;

    if (fileContext->Lock == NULL) {
        return;   // create never completed
    }

    WdfWaitLockAcquire(fileContext->Lock, NULL);

    // 1. User mappings (must precede DMA buffer teardown; runs in the closing
    //    process context).
    while (!IsListEmpty(&fileContext->Mappings)) {
        PTT_USER_MAPPING mapping = CONTAINING_RECORD(
            RemoveHeadList(&fileContext->Mappings), TT_USER_MAPPING, Entry);

        TtDestroyUserMapping(mapping);
    }

    // 2. Pinnings (iATU teardown inside).
    while (!IsListEmpty(&fileContext->Pinnings)) {
        PTT_PINNING pinning = CONTAINING_RECORD(
            RemoveHeadList(&fileContext->Pinnings), TT_PINNING, Entry);

        TtDestroyPinning(Context, pinning);
    }

    // 3. DMA buffers: aperture down first, then the memory (DD-8).
    for (i = 0; i < TENSTORRENT_MAX_DMA_BUFS; i++) {
        PTT_DMABUF dmabuf = fileContext->DmaBufs[i];

        if (dmabuf != NULL) {
            fileContext->DmaBufs[i] = NULL;
            TtTeardownNocDma(Context, dmabuf->IatuRegion);
            WdfObjectDelete(dmabuf->Buffer);
            ExFreePoolWithTag(dmabuf, TT_TAG_DMABUF);
        }
    }

    WdfWaitLockRelease(fileContext->Lock);

    // 4. TLB windows owned by this handle.
    WdfWaitLockAcquire(Context->TlbLock, NULL);
    for (i = 0; i < TT_TLB_TOTAL; i++) {
        if (Context->TlbOwner[i] == FileObject) {
            Context->TlbUsed[i] = FALSE;
            Context->TlbOwner[i] = NULL;
        }
    }
    WdfWaitLockRelease(Context->TlbLock);

    TraceLoggingWrite(g_TtTraceProvider, "FileCleanup");
}
