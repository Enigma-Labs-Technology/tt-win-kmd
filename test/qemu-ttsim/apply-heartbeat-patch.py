#!/usr/bin/env python3
"""Test-rig patch: add TELEMETRY_TIMER_HEARTBEAT (tag 32) to ttsim's Blackhole
telemetry table, lazily refreshed from the simulation clock on CSM reads.

Rationale: stock ttsim publishes a static telemetry table without tag 32, but
tt-kmd's tt_heartbeat surface (porting-spec M2 acceptance: "heartbeat counter
observed advancing on ttsim") needs a monotonically advancing value. Real CMFW
increments tag 32 continuously; this derives it from g_clock (>>20), preserving
the only semantic tt-kmd relies on: it advances while the chip runs.

Anchor-based (not a line-number diff) so it fails loudly on upstream drift.
Applied by build-ttsim.sh to a pristine COPY of ../ttsim; never modifies the
user's checkout. Idempotent.
"""
import sys

MARK = "test-rig heartbeat patch"


def patch_libttsim(path: str) -> None:
    """BH strided-TLB registers (BAR0 0x1FC009D8..0x1FC00A57, 32 x 4B) are real
    hardware that tt-kmd writes (strided-config clear for windows < 32,
    tt-kmd/blackhole.c:192-196) but stock ttsim doesn't model them — the write
    is a fatal UnimplementedFunctionality. Accept and ignore them (the driver
    only ever writes 0, and strided translation is unused by the port)."""
    src = open(path).read()
    if MARK in src:
        print("libttsim already patched")
        return

    anchor = """                case 0x1FC00000 ... 0x1FC009D4:
                    TTSIM_VERIFY(!(offset & 3), UnsupportedFunctionality, "bar0: misaligned offset=0x%x", offset);
                    TTSIM_VERIFY(size == 4, UnsupportedFunctionality, "bar0: offset=0x%x size=%d", offset, size);
                    tlb_cfg_wr32((offset - 0x1FC00000) / 4, mem_rd<uint32_t>(p));
                    break;"""
    if src.count(anchor) != 1:
        sys.exit("ANCHOR MISSING: BH tlb_cfg write case in libttsim.cpp")
    src = src.replace(anchor, anchor + f"""
                case 0x1FC009D8 ... 0x1FC00A57: // strided TLB regs ({MARK})
                    TTSIM_VERIFY(size == 4 && mem_rd<uint32_t>(p) == 0, UnimplementedFunctionality,
                        "strided tlb cfg: offset=0x%x size=%d", offset, size);
                    break;""")
    open(path, "w").write(src)
    print("patched", path)


def patch(path: str) -> None:
    src = open(path).read()
    if MARK in src:
        print("already patched")
        return

    def insert_after(anchor: str, addition: str, count: int = 1) -> None:
        nonlocal src
        if src.count(anchor) < count:
            sys.exit(f"ANCHOR MISSING ({count} needed): {anchor!r}")
        src = src.replace(anchor, anchor + addition, count)

    # 1. File-scope slot-offset variable, hooked near the telemetry defines.
    insert_after(
        "#define ARC_TELEMETRY_TABLE_CSM_OFFSET 0x100",
        f"\n[[maybe_unused]] static uint32_t g_heartbeat_value_off; // {MARK}",
    )

    # 2. Add tag 32 to the Blackhole telemetry table.
    insert_after(
        "{62, 1 + g_current_chip_id},    // ASIC_ID_LOW",
        f"\n        {{32, 0x0}},                     // TIMER_HEARTBEAT ({MARK})",
    )

    # 3. Capture the tag-32 value slot offset while the table is built.
    insert_after(
        "mem_wr<uint32_t>(&g_a_tile.csm[ARC_TELEMETRY_VALUES_CSM_OFFSET + 4 * i], telem[i].value);",
        f"""
#if TT_ARCH_VERSION == 1
        if (telem[i].tag == 32) // {MARK}
            g_heartbeat_value_off = ARC_TELEMETRY_VALUES_CSM_OFFSET + 4 * i;
#endif""",
    )

    # 4. Lazy refresh on the CSM *read* path only: find the read-direction
    #    memcpy (dst = caller buffer) and prepend the refresh.
    read_memcpy = "memcpy(p, &g_a_tile.csm[csm_offset], size);"
    if src.count(read_memcpy) != 1:
        sys.exit(f"ANCHOR MISSING/AMBIGUOUS: {read_memcpy!r}")
    src = src.replace(
        read_memcpy,
        f"""#if TT_ARCH_VERSION == 1
            if (g_heartbeat_value_off && csm_offset <= g_heartbeat_value_off &&
                g_heartbeat_value_off < csm_offset + size) // {MARK}
                mem_wr<uint32_t>(&g_a_tile.csm[g_heartbeat_value_off], (uint32_t)(g_clock >> 20));
#endif
            {read_memcpy}""",
    )

    open(path, "w").write(src)
    print("patched", path)


if __name__ == "__main__":
    import os
    base = os.path.dirname(sys.argv[1] if len(sys.argv) > 1 else "src/tile.cpp")
    patch(os.path.join(base, "tile.cpp"))
    patch_libttsim(os.path.join(base, "libttsim.cpp"))
