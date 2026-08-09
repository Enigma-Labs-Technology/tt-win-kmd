#!/usr/bin/bash
# Focused regression tests for the ttsim publication/launch trust boundary.
set -euo pipefail
umask 077
PATH=/usr/bin:/bin
export PATH LC_ALL=C

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd -P)
HELPER="$SCRIPT_DIR/ttsim-provenance.sh"
QEMU_WRAPPER="$SCRIPT_DIR/qemu/build/qemu-system-x86_64"
# shellcheck source=ttsim-provenance.sh
source "$HELPER"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

expect_failure() {
    local label=$1
    shift
    if "$@" >/dev/null 2>&1; then
        fail "$label unexpectedly succeeded"
    fi
}

build_output=$("$SCRIPT_DIR/build-ttsim.sh" 2>&1) || {
    printf '%s\n' "$build_output" >&2
    fail "exact ttsim build/cache validation failed"
}
/usr/bin/grep -Fq 'Reused exact completed ttsim artifact.' <<< "$build_output" || \
    fail "second-stage provenance test expected an exact cache hit"
library=$(/usr/bin/awk -F= '$1 == "TTSIM_LIB" { value=$2 } END { print value }' \
    <<< "$build_output")
[[ -n "$library" ]] || fail "build output did not publish TTSIM_LIB"
relative_build_output=$(
    cd "$REPO_ROOT"
    test/qemu-ttsim/build-ttsim.sh 2>&1
) || {
    /usr/bin/printf '%s\n' "$relative_build_output" >&2
    fail "relative-path ttsim build invocation lost its canonical script identity"
}
/usr/bin/grep -Fq 'Reused exact completed ttsim artifact.' \
    <<< "$relative_build_output" || \
    fail "relative-path ttsim invocation did not reuse the canonical exact build"
relative_library=$(/usr/bin/awk -F= \
    '$1 == "TTSIM_LIB" { value=$2 } END { print value }' \
    <<< "$relative_build_output")
[[ "$relative_library" == "$library" ]] || \
    fail "relative-path ttsim invocation changed the published artifact"
manifest="${library%/*}/$TTSIM_MANIFEST_NAME"
ttsim_validate_artifact "$manifest" "$library" || \
    fail "published artifact did not validate"
library_hash=$TTSIM_VALIDATED_SHA256
build_input_hash=${TTSIM_MANIFEST[build_input_sha256]}
publication_lock=$TTSIM_VALIDATED_LOCK

# Simulate an interrupted publication that left an immutable cache ref pointing
# at an absent content hash.  The builder must remove only that exact ref,
# rebuild/reuse the validated content-addressed artifact, and repair the ref.
cache_ref="$SCRIPT_DIR/ttsim-work/cache/$build_input_hash.ref"
[[ -f "$cache_ref" && ! -L "$cache_ref" ]] || \
    fail "exact ttsim cache reference is unavailable for recovery test"
/usr/bin/chmod 0600 "$cache_ref"
/usr/bin/printf '%064d\n' 0 | /usr/bin/tr '0' 'f' > "$cache_ref"
/usr/bin/chmod 0444 "$cache_ref"
cache_recovery_output=$("$SCRIPT_DIR/build-ttsim.sh" 2>&1) || {
    /usr/bin/printf '%s\n' "$cache_recovery_output" >&2
    fail "builder did not recover an interrupted exact-cache reference"
}
/usr/bin/grep -Fq 'Built and atomically published exact ttsim artifact.' \
    <<< "$cache_recovery_output" || {
        /usr/bin/printf '%s\n' "$cache_recovery_output" >&2
        fail "invalid exact-cache reference did not take the rebuild path"
    }
recovered_library=$(/usr/bin/awk -F= \
    '$1 == "TTSIM_LIB" { value=$2 } END { print value }' \
    <<< "$cache_recovery_output")
[[ "$recovered_library" == "$library" &&
   "$(<"$cache_ref")" == "$library_hash" ]] || \
    fail "cache-ref recovery changed the exact artifact or failed to repair its ref"

temporary=$(/usr/bin/mktemp -d /tmp/ttsim-provenance-test.XXXXXXXX)
qemu_lock_alias=''
fresh_qemu_pid=''
QEMU_ENV_HOME=${HOME:-}
QEMU_ENV_USER=$(/usr/bin/id -un)
[[ "$QEMU_ENV_HOME" == /* && -d "$QEMU_ENV_HOME" ]] || \
    fail "HOME is unsuitable for the sanitized QEMU test environment"
QEMU_CLEAN_ENV=(
    /usr/bin/env -i PATH=/usr/bin:/bin "HOME=$QEMU_ENV_HOME"
    "USER=$QEMU_ENV_USER" LANG=C LC_ALL=C TZ=UTC TMPDIR=/tmp
)
cleanup() {
    local status=$?
    trap - EXIT
    case "$temporary" in
        /tmp/ttsim-provenance-test.*) /usr/bin/chmod -R u+w "$temporary" 2>/dev/null || true
            /usr/bin/rm -rf -- "$temporary" ;;
        *) status=1 ;;
    esac
    if [[ -n "$qemu_lock_alias" &&
          "$qemu_lock_alias" == "$SCRIPT_DIR"/qemu/build/.wrapper-lock-alias.* ]]; then
        /usr/bin/rm -f -- "$qemu_lock_alias" || status=1
    fi
    if [[ -n "$fresh_qemu_pid" ]] && /usr/bin/kill -0 "$fresh_qemu_pid" 2>/dev/null; then
        /usr/bin/kill "$fresh_qemu_pid" 2>/dev/null || true
        wait "$fresh_qemu_pid" 2>/dev/null || true
    fi
    exit "$status"
}
trap cleanup EXIT

# Two callers racing the exact same build must serialize on the generated
# build lock and both observe the one completed immutable publication.
"$SCRIPT_DIR/build-ttsim.sh" > "$temporary/concurrent-a.log" 2>&1 &
build_a=$!
"$SCRIPT_DIR/build-ttsim.sh" > "$temporary/concurrent-b.log" 2>&1 &
build_b=$!
if ! wait "$build_a"; then
    /usr/bin/cat "$temporary/concurrent-a.log" >&2
    fail "first concurrent exact-cache caller failed"
fi
if ! wait "$build_b"; then
    /usr/bin/cat "$temporary/concurrent-b.log" >&2
    fail "second concurrent exact-cache caller failed"
fi
for log in "$temporary/concurrent-a.log" "$temporary/concurrent-b.log"; do
    /usr/bin/grep -Fq 'Reused exact completed ttsim artifact.' "$log" || \
        fail "concurrent caller did not reuse the completed exact artifact"
    /usr/bin/grep -Fq "Library SHA256: $library_hash" "$log" || \
        fail "concurrent caller observed a different library identity"
done

make_fixture() {
    local name=$1 root destination
    root="$temporary/$name/artifacts"
    destination="$root/sha256/$library_hash/$build_input_hash"
    /usr/bin/mkdir -p -- "$destination"
    /usr/bin/cp -- "$library" "$destination/libttsim.so"
    /usr/bin/cp -- "$manifest" "$destination/$TTSIM_MANIFEST_NAME"
    /usr/bin/touch -- "$root/.publication.lock"
    /usr/bin/chmod 0444 -- "$destination/libttsim.so" \
        "$destination/$TTSIM_MANIFEST_NAME"
    /usr/bin/chmod 0555 -- "$destination"
    printf '%s\n' "$destination"
}

fixture=$(make_fixture valid)
ttsim_validate_artifact "$fixture/$TTSIM_MANIFEST_NAME" \
    "$fixture/libttsim.so" || fail "equivalent immutable fixture was rejected"

fixture=$(make_fixture writable)
/usr/bin/chmod 0644 -- "$fixture/libttsim.so"
expect_failure "writable content-addressed library" \
    ttsim_validate_artifact "$fixture/$TTSIM_MANIFEST_NAME" "$fixture/libttsim.so"

fixture=$(make_fixture corrupt)
/usr/bin/chmod 0644 -- "$fixture/libttsim.so"
printf 'X' >> "$fixture/libttsim.so"
/usr/bin/chmod 0444 -- "$fixture/libttsim.so"
expect_failure "corrupted content-addressed library" \
    ttsim_validate_artifact "$fixture/$TTSIM_MANIFEST_NAME" "$fixture/libttsim.so"

fixture=$(make_fixture malformed)
/usr/bin/chmod 0644 -- "$fixture/$TTSIM_MANIFEST_NAME"
printf 'unexpected=field\n' >> "$fixture/$TTSIM_MANIFEST_NAME"
/usr/bin/chmod 0444 -- "$fixture/$TTSIM_MANIFEST_NAME"
expect_failure "manifest with trailing fields" \
    ttsim_validate_artifact "$fixture/$TTSIM_MANIFEST_NAME" "$fixture/libttsim.so"

fixture=$(make_fixture hardlinked-library)
/usr/bin/ln -- "$fixture/libttsim.so" "$temporary/hardlinked-library-alias"
expect_failure "multiply linked content-addressed library" \
    ttsim_validate_artifact "$fixture/$TTSIM_MANIFEST_NAME" "$fixture/libttsim.so"

fixture=$(make_fixture hardlinked-manifest)
/usr/bin/ln -- "$fixture/$TTSIM_MANIFEST_NAME" "$temporary/hardlinked-manifest-alias"
expect_failure "multiply linked content-addressed manifest" \
    ttsim_validate_artifact "$fixture/$TTSIM_MANIFEST_NAME" "$fixture/libttsim.so"

fixture=$(make_fixture wrong-address)
wrong="$temporary/wrong-address/artifacts/sha256/$library_hash/$(printf wrong | /usr/bin/sha256sum | /usr/bin/cut -d ' ' -f1)"
/usr/bin/mv -- "$fixture" "$wrong"
expect_failure "artifact under the wrong build-input address" \
    ttsim_validate_artifact "$wrong/$TTSIM_MANIFEST_NAME" "$wrong/libttsim.so"

# The launcher's shared lock must exclude an atomic publisher while manifest
# and pathname identities are validated.  The launcher then pins the exact
# library inode and releases this coordination lock before the long-lived VM.
exec 8<"$publication_lock"
/usr/bin/flock -s 8
ttsim_validate_open_lock "$publication_lock" 8 || \
    fail "opened publication lock did not pass identity validation"
if /usr/bin/flock -n "$publication_lock" -c /usr/bin/true; then
    fail "exclusive publisher lock bypassed an active reader"
fi
/usr/bin/flock -u 8
exec 8>&-

lock_fixture="$temporary/publication.lock"
/usr/bin/touch -- "$lock_fixture"
/usr/bin/chmod 0600 -- "$lock_fixture"
/usr/bin/ln -- "$lock_fixture" "$temporary/publication-lock-alias"
exec 8<"$lock_fixture"
/usr/bin/flock -s 8
expect_failure "multiply linked publication lock" \
    ttsim_validate_open_lock "$lock_fixture" 8
/usr/bin/flock -u 8
exec 8>&-

# Prove PATH independence when the wrapper is entered through the documented
# sanitized environment.  Direct execution of a Bash wrapper assumes a trusted
# process environment because BASH_ENV/loader hooks run before its first line.
[[ -f "$QEMU_WRAPPER" && ! -L "$QEMU_WRAPPER" ]] || \
    fail "validated QEMU wrapper is missing"
/usr/bin/grep -Fq 'exec "/proc/self/fd/$real_fd"' "$QEMU_WRAPPER" || \
    fail "QEMU wrapper does not execute the already-hashed binary inode"
wrapper_before=$(/usr/bin/sha256sum "$QEMU_WRAPPER" | /usr/bin/cut -d ' ' -f1)
relative_qemu_output=$(
    cd "$REPO_ROOT"
    test/qemu-ttsim/build-qemu.sh 2>&1
) || {
    /usr/bin/printf '%s\n' "$relative_qemu_output" >&2
    fail "relative-path QEMU build invocation lost its canonical script identity"
}
/usr/bin/grep -Fq 'Reused exact completed cache:' <<< "$relative_qemu_output" || \
    fail "relative-path QEMU invocation did not reuse the canonical exact cache"
wrapper_after=$(/usr/bin/sha256sum "$QEMU_WRAPPER" | /usr/bin/cut -d ' ' -f1)
[[ "$wrapper_after" == "$wrapper_before" ]] || \
    fail "relative-path QEMU invocation changed the canonical wrapper identity"
qemu_publication_lock="$SCRIPT_DIR/qemu/build/.tt-win-kmd-wrapper.lock"
qemu_lock_alias="$SCRIPT_DIR/qemu/build/.wrapper-lock-alias.$$"
/usr/bin/ln -- "$qemu_publication_lock" "$qemu_lock_alias"
set +e
hardlink_output=$("$SCRIPT_DIR/build-qemu.sh" 2>&1)
hardlink_status=$?
set -e
/usr/bin/rm -- "$qemu_lock_alias"
qemu_lock_alias=''
[[ "$hardlink_status" -ne 0 ]] || fail "hard-linked QEMU publication lock was accepted"
/usr/bin/grep -Fq 'single-link 0600 file' <<< "$hardlink_output" || {
    printf '%s\n' "$hardlink_output" >&2
    fail "QEMU publication-lock hard-link rejection was not explicit"
}
/usr/bin/mkdir -- "$temporary/hostile-bin"
printf '#!/bin/sh\nexit 99\n' > "$temporary/hostile-bin/sha256sum"
printf '#!/bin/sh\nexit 99\n' > "$temporary/hostile-bin/bash"
/usr/bin/chmod 0700 "$temporary/hostile-bin/sha256sum" "$temporary/hostile-bin/bash"
printf 'printf "WRAPPER_BASH_ENV_LEAK\\n" >&2\n' > "$temporary/bash-env-probe"
set +e
path_probe_output=$(
    BASH_ENV="$temporary/bash-env-probe" \
        /usr/bin/env -i PATH="$temporary/hostile-bin" \
        "HOME=$QEMU_ENV_HOME" "USER=$QEMU_ENV_USER" \
        LANG=C LC_ALL=C TZ=UTC TMPDIR=/tmp \
        "$QEMU_WRAPPER" --version 2>&1
)
path_probe_status=$?
set -e
[[ "$path_probe_status" -eq 0 &&
   "$path_probe_output" == *'QEMU emulator version 11.0.2'* &&
   "$path_probe_output" != *'WRAPPER_BASH_ENV_LEAK'* ]] || {
    /usr/bin/printf '%s\n' "$path_probe_output" >&2
    fail "sanitized QEMU wrapper launch trusted PATH or BASH_ENV"
}

# A stable root directory is insufficient if QEMU can reopen a writable BIOS
# descendant after hashing.  Exercise the generated wrapper's recursive mode,
# ownership, and link-count gate without mutating the real cache.
/usr/bin/mkdir -- "$temporary/writable-pc-bios"
/usr/bin/printf 'fixture\n' > "$temporary/writable-pc-bios/firmware.bin"
/usr/bin/chmod 0666 "$temporary/writable-pc-bios/firmware.bin"
/usr/bin/cp -- "$QEMU_WRAPPER" "$temporary/writable-bios-wrapper"
/usr/bin/chmod 0700 "$temporary/writable-bios-wrapper"
/usr/bin/sed -i \
    -e "s|^pc_bios=.*|pc_bios=$temporary/writable-pc-bios|" \
    -e 's|^expected_pc_bios=.*|expected_pc_bios=0000000000000000000000000000000000000000000000000000000000000000|' \
    "$temporary/writable-bios-wrapper"
set +e
writable_bios_output=$("${QEMU_CLEAN_ENV[@]}" \
    "$temporary/writable-bios-wrapper" --version 2>&1)
writable_bios_status=$?
set -e
[[ "$writable_bios_status" -ne 0 &&
   "$writable_bios_output" == *'unowned, writable, or multiply-linked file'* ]] || {
    /usr/bin/printf '%s\n' "$writable_bios_output" >&2
    fail "generated wrapper accepted a writable pc-bios descendant"
}

set +e
only_migratable_output=$("${QEMU_CLEAN_ENV[@]}" "$QEMU_WRAPPER" \
    -only-migratable -machine q35 -accel qtest -nodefaults -display none -S \
    -device "ttsim-bh,bus=pcie.0,addr=06.0,ttsim-lib=$library" 2>&1)
only_migratable_status=$?
set -e
[[ "$only_migratable_status" -ne 0 &&
   "$only_migratable_output" == \
       *'ttsim-bh uses process-local libttsim state and cannot be migrated'* ]] || {
    /usr/bin/printf '%s\n' "$only_migratable_output" >&2
    fail "-only-migratable accepted the process-local ttsim device"
}

# Read the exact simulator-only fresh-process marker through PCI configuration
# I/O, consume it exactly as the Debug driver does, and prove ordinary timer
# progress cannot re-arm it.  A second QEMU process must receive a new marker.
start_fresh_epoch_qemu() {
    local suffix=$1
    fresh_qmp="$temporary/fresh-$suffix-qmp.sock"
    fresh_qtest="$temporary/fresh-$suffix-qtest.sock"
    fresh_log="$temporary/fresh-$suffix.log"
    "${QEMU_CLEAN_ENV[@]}" "$QEMU_WRAPPER" \
        -machine q35 -accel qtest -nodefaults -display none -S \
        -sandbox on,obsolete=deny,elevateprivileges=deny,spawn=deny,resourcecontrol=deny \
        -qmp "unix:$fresh_qmp,server=on,wait=off" \
        -qtest "unix:$fresh_qtest,server=on,wait=off" \
        -device "ttsim-bh,bus=pcie.0,addr=06.0,ttsim-lib=$library" \
        > "$fresh_log" 2>&1 &
    fresh_qemu_pid=$!
    for _ in $(/usr/bin/seq 1 100); do
        [[ -S "$fresh_qmp" && -S "$fresh_qtest" ]] && return 0
        /usr/bin/kill -0 "$fresh_qemu_pid" 2>/dev/null || {
            /usr/bin/cat "$fresh_log" >&2
            return 1
        }
        /usr/bin/sleep 0.05
    done
    /usr/bin/cat "$fresh_log" >&2
    return 1
}

stop_fresh_epoch_qemu() {
    local qmp_output status
    set +e
    qmp_output=$(
        /usr/bin/printf '%s\n' \
            '{"execute":"qmp_capabilities"}' \
            '{"execute":"quit"}' |
            /usr/bin/timeout 5 /usr/bin/socat - "UNIX-CONNECT:$fresh_qmp" 2>/dev/null
    )
    set -e
    # QEMU may close the Unix socket immediately after acknowledging quit,
    # which makes socat report ECONNRESET.  The exact QMP replies and child
    # exit status below, not that transport teardown, are authoritative.
    [[ $(/usr/bin/grep -Fc '"return": {}' <<< "$qmp_output" || true) -ge 2 ]] || {
        /usr/bin/printf '%s\n' "$qmp_output" >&2
        fail "fresh-epoch QMP did not acknowledge capabilities and quit"
    }
    if wait "$fresh_qemu_pid"; then
        status=0
    else
        status=$?
    fi
    fresh_qemu_pid=''
    [[ "$status" -eq 0 ]] || {
        /usr/bin/cat "$fresh_log" >&2
        fail "fresh-epoch QEMU exited with status $status"
    }
}

start_fresh_epoch_qemu first || fail "first fresh-epoch QEMU did not start"
# BAR0 is placed at 0xC0000000.  The 0xDFC0096C triplet programs Blackhole's
# driver-reserved 2 MiB TLB window 201 for ARC (8,0), first at CSM 0x10000000
# and then APB 0x80000000; 0xD9200000 is that window's guest BAR alias.
fresh_first=$(
    /usr/bin/printf '%s\n' \
        'outl 0xcf8 0x80003040' \
        'inl 0xcfc' \
        'outw 0xcfe 0x0000' \
        'outl 0xcf8 0x80003010' \
        'outl 0xcfc 0xc0000000' \
        'outl 0xcf8 0x80003014' \
        'outl 0xcfc 0x00000000' \
        'outl 0xcf8 0x80003004' \
        'outw 0xcfc 0x0006' \
        'outl 0xcf8 0x80003040' \
        'outw 0xcfc 0xa55a' \
        'inl 0xcfc' \
        'writel 0xdfc0096c 0x00000080' \
        'writel 0xdfc00970 0x00004000' \
        'writel 0xdfc00974 0x00000040' \
        'writel 0xd9200420 0x00000090' \
        'writel 0xd9200400 0x00000001' \
        'writel 0xdfc0096c 0x00000400' \
        'writel 0xd92b0000 0x00000000' \
        'inl 0xcfc' \
        'writel 0xdfc0096c 0x00000080' \
        'writel 0xd9200440 0x00000056' \
        'writel 0xd9200400 0x00000002' \
        'writel 0xdfc0096c 0x00000400' \
        'writel 0xd92b0000 0x00000000' \
        'inl 0xcfc' \
        'outw 0xcfe 0xc35a' \
        'inw 0xcfe' \
        'outb 0xcff 0xc3' \
        'inw 0xcfe' \
        'outb 0xcfe 0x5a' \
        'inw 0xcfe' |
        /usr/bin/timeout 5 /usr/bin/socat - "UNIX-CONNECT:$fresh_qtest"
) || {
    /usr/bin/cat "$fresh_log" >&2
    fail "fresh-epoch PCI configuration exchange failed"
}
[[ $(/usr/bin/grep -Fxc 'OK 0xc35a0000' <<< "$fresh_first" || true) -eq 1 &&
   $(/usr/bin/grep -Fxc 'OK 0xa55a' <<< "$fresh_first" || true) -eq 2 &&
   $(/usr/bin/grep -Fxc 'OK 0x0000' <<< "$fresh_first" || true) -eq 4 ]] || {
    /usr/bin/printf '%s\n' "$fresh_first" >&2
    fail "non-reset/reset ARC epoch ordering, fresh-marker consumption, or irreversible latch is incorrect"
}
migration_target="$temporary/forbidden-migration.bin"
fresh_migration_qmp=$(
    /usr/bin/printf '%s\n' \
        '{"execute":"qmp_capabilities"}' \
        "{\"execute\":\"migrate\",\"arguments\":{\"uri\":\"file:$migration_target\"}}" |
        /usr/bin/timeout 5 /usr/bin/socat - "UNIX-CONNECT:$fresh_qmp"
) || {
    /usr/bin/cat "$fresh_log" >&2
    fail "could not exercise the ttsim migration blocker"
}
[[ $(/usr/bin/grep -Fc '"return": {}' <<< "$fresh_migration_qmp" || true) -eq 1 &&
   $(/usr/bin/grep -Fc 'ttsim-bh uses process-local libttsim state and cannot be migrated' \
       <<< "$fresh_migration_qmp" || true) -eq 1 &&
   ! -e "$migration_target" ]] || {
    /usr/bin/printf '%s\n' "$fresh_migration_qmp" >&2
    fail "live migration could bypass the fresh-process simulator contract"
}
fresh_reset_qmp=$(
    /usr/bin/printf '%s\n' \
        '{"execute":"qmp_capabilities"}' \
        '{"execute":"system_reset"}' |
        /usr/bin/timeout 5 /usr/bin/socat - "UNIX-CONNECT:$fresh_qmp"
) || {
    /usr/bin/cat "$fresh_log" >&2
    fail "pre-run construction reset failed in the fresh-epoch test"
}
[[ $(/usr/bin/grep -Fc '"return": {}' <<< "$fresh_reset_qmp" || true) -ge 2 ]] || {
    /usr/bin/printf '%s\n' "$fresh_reset_qmp" >&2
    fail "QMP did not acknowledge the pre-run construction reset"
}
fresh_after_reset=$(
    /usr/bin/printf '%s\n' 'outl 0xcf8 0x80003040' 'inl 0xcfc' |
        /usr/bin/timeout 5 /usr/bin/socat - "UNIX-CONNECT:$fresh_qtest"
) || {
    /usr/bin/cat "$fresh_log" >&2
    fail "could not re-read the consumed epoch after construction reset"
}
[[ $(/usr/bin/grep -Fxc 'OK 0x0000' <<< "$fresh_after_reset" || true) -eq 1 ]] || {
    /usr/bin/printf '%s\n' "$fresh_after_reset" >&2
    fail "a construction reset incorrectly re-armed the consumed fresh epoch"
}
stop_fresh_epoch_qemu

start_fresh_epoch_qemu second || fail "second fresh-epoch QEMU did not start"
fresh_second=$(
    /usr/bin/printf '%s\n' \
        'outl 0xcf8 0x80003040' 'inl 0xcfc' \
        'outw 0xcfe 0x0000' 'inl 0xcfc' |
        /usr/bin/timeout 5 /usr/bin/socat - "UNIX-CONNECT:$fresh_qtest"
) || {
    /usr/bin/cat "$fresh_log" >&2
    fail "second fresh-epoch PCI configuration exchange failed"
}
[[ $(/usr/bin/grep -Fxc 'OK 0xc35a0000' <<< "$fresh_second" || true) -eq 1 &&
   $(/usr/bin/grep -Fxc 'OK 0x0000' <<< "$fresh_second" || true) -eq 1 ]] || {
    /usr/bin/printf '%s\n' "$fresh_second" >&2
    fail "a newly created QEMU/libttsim process did not issue a fresh marker"
}
set +e
fresh_started_qmp=$(
    /usr/bin/printf '%s\n' \
        '{"execute":"qmp_capabilities"}' \
        '{"execute":"cont"}' \
        '{"execute":"system_reset"}' |
        /usr/bin/timeout 5 /usr/bin/socat - "UNIX-CONNECT:$fresh_qmp" 2>/dev/null
)
set -e
for _ in $(/usr/bin/seq 1 100); do
    /usr/bin/kill -0 "$fresh_qemu_pid" 2>/dev/null || break
    /usr/bin/sleep 0.05
done
if /usr/bin/kill -0 "$fresh_qemu_pid" 2>/dev/null; then
    /usr/bin/printf '%s\n' "$fresh_started_qmp" >&2
    /usr/bin/cat "$fresh_log" >&2
    fail "a reset after VM start did not terminate the consumed simulator process"
fi
if wait "$fresh_qemu_pid"; then
    fresh_started_status=0
else
    fresh_started_status=$?
fi
fresh_qemu_pid=''
[[ "$fresh_started_status" -eq 1 &&
   $(/usr/bin/grep -Fc '"return": {}' <<< "$fresh_started_qmp" || true) -ge 2 &&
   $(/usr/bin/grep -Fc 'in-process device/system reset is unsupported' "$fresh_log" || true) -eq 1 ]] || {
    /usr/bin/printf '%s\n' "$fresh_started_qmp" >&2
    /usr/bin/cat "$fresh_log" >&2
    fail "post-start reset did not fail closed through fresh-process termination"
}

# Finally prove that an already-open descriptor survives the wrapper's exec
# and is the object the ttsim device actually dlopens.  QMP quit gives a clean,
# bounded realize/teardown smoke without touching the installed VM.
exec 7<"$library"
opened_hash=$(/usr/bin/sha256sum "/proc/$$/fd/7" | /usr/bin/cut -d ' ' -f1)
[[ "$opened_hash" == "$library_hash" ]] || fail "opened smoke-test inode changed"
qmp_output=$(
    printf '%s\n' \
        '{"execute":"qmp_capabilities"}' \
        '{"execute":"quit"}' |
        "${QEMU_CLEAN_ENV[@]}" /usr/bin/timeout 30 "$QEMU_WRAPPER" \
            -machine q35 -accel tcg -nodefaults -display none -S \
            -sandbox on,obsolete=deny,elevateprivileges=deny,spawn=deny,resourcecontrol=deny \
            -qmp stdio -device "ttsim-bh,ttsim-lib=/proc/self/fd/7" 2>&1
) || {
    printf '%s\n' "$qmp_output" >&2
    fail "QEMU could not realize the validated inherited-FD ttsim artifact"
}
[[ $(/usr/bin/grep -Fc '"return": {}' <<< "$qmp_output" || true) -ge 2 ]] || {
    printf '%s\n' "$qmp_output" >&2
    fail "QMP did not confirm clean realize and quit"
}
exec 7>&-

# Exercise the production run-vm.sh contract in an isolated disposable VM
# directory.  This uses a real qcow2 header and OVMF variable template, but no
# user disk, snapshot, or live qmp.sock from test/vm is touched.
/usr/bin/mkdir -p -- "$temporary/run-vm/test/vm" \
    "$temporary/run-vm/test/qemu-ttsim/qemu/build"
/usr/bin/cp -- "$SCRIPT_DIR/../vm/run-vm.sh" \
    "$temporary/run-vm/test/vm/run-vm.sh"
/usr/bin/cp -- "$HELPER" \
    "$temporary/run-vm/test/qemu-ttsim/ttsim-provenance.sh"
/usr/bin/install -m 0555 -- "$QEMU_WRAPPER" \
    "$temporary/run-vm/test/qemu-ttsim/qemu/build/qemu-system-x86_64"
/usr/bin/install -m 0444 -- "$SCRIPT_DIR/qemu/build/.tt-win-kmd-wrapper-complete" \
    "$temporary/run-vm/test/qemu-ttsim/qemu/build/.tt-win-kmd-wrapper-complete"
/usr/bin/touch -- "$temporary/run-vm/test/qemu-ttsim/qemu/build/.tt-win-kmd-wrapper.lock"
/usr/bin/chmod 0600 -- "$temporary/run-vm/test/qemu-ttsim/qemu/build/.tt-win-kmd-wrapper.lock"
select_ovmf_source() {
    local candidate
    for candidate in "$@"; do
        if [[ -e "$candidate" || -L "$candidate" ]]; then
            /usr/bin/realpath -e -- "$candidate"
            return 0
        fi
    done
    return 1
}
ovmf_code_source=$(select_ovmf_source \
    /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd) ||
    fail "OVMF code fixture is unavailable"
ovmf_vars_source=$(select_ovmf_source \
    /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd) ||
    fail "OVMF variable fixture is unavailable"
ovmf_code_sha256=$(/usr/bin/sha256sum "$ovmf_code_source" |
    /usr/bin/cut -d ' ' -f1)
ovmf_vars_sha256=$(/usr/bin/sha256sum "$ovmf_vars_source" |
    /usr/bin/cut -d ' ' -f1)
(
    cd "$temporary/run-vm/test/vm"
    /usr/bin/qemu-img create -q -f qcow2 win11.qcow2 1G
    /usr/bin/cp "$ovmf_vars_source" OVMF_VARS.runtime.fd
    /usr/bin/printf '%s\n' \
        'schema=disposable-provenance-smoke' \
        "ovmf_code_path=$ovmf_code_source" \
        "ovmf_code_sha256=$ovmf_code_sha256" \
        "ovmf_vars_source_path=$ovmf_vars_source" \
        "ovmf_vars_source_sha256=$ovmf_vars_sha256" \
        > .vm-bootstrap-platform
)
set +e
/usr/bin/timeout --signal=TERM --kill-after=5 8 \
    /usr/bin/env BASH_ENV="$temporary/bash-env-probe" \
    MEM=512 SMP=1 SSH_PORT=22999 \
    OVMF_CODE_SRC="$ovmf_code_source" OVMF_VARS_SRC="$ovmf_vars_source" \
    EXTRA_DEVICES="-device ttsim-bh,ttsim-lib=$library" \
    /usr/bin/bash "$temporary/run-vm/test/vm/run-vm.sh" \
    > "$temporary/run-vm.stdout" 2> "$temporary/run-vm.stderr"
run_status=$?
set -e
[[ "$run_status" -eq 0 || "$run_status" -eq 124 || "$run_status" -eq 143 ]] || {
    /usr/bin/cat "$temporary/run-vm.stdout" "$temporary/run-vm.stderr" >&2
    fail "isolated run-vm provenance launch failed with $run_status"
}
[[ $(/usr/bin/grep -Fxc 'WRAPPER_BASH_ENV_LEAK' \
        "$temporary/run-vm.stderr" || true) -eq 1 ]] || {
    /usr/bin/cat "$temporary/run-vm.stdout" "$temporary/run-vm.stderr" >&2
    fail "BASH_ENV reached a pinned wrapper child instead of only the caller shell"
}
platform="$temporary/run-vm/test/vm/.vm-current-platform"
[[ -f "$platform" && ! -L "$platform" ]] || {
    /usr/bin/cat "$temporary/run-vm.stdout" "$temporary/run-vm.stderr" >&2
    fail "run-vm did not survive validation long enough to publish its platform identity"
}
for expected in \
        "ttsim_lib_path=$library" \
        "ttsim_lib_sha256=$library_hash" \
        "ttsim_manifest_path=$manifest" \
        "ttsim_manifest_sha256=$TTSIM_VALIDATED_MANIFEST_SHA256" \
        "ttsim_build_input_sha256=$build_input_hash"; do
    /usr/bin/grep -Fqx "$expected" "$platform" || \
        fail "run-vm platform manifest omitted: $expected"
done
[[ ! -e "$temporary/run-vm/test/vm/.vm-state.lock" &&
   ! -e "$temporary/run-vm/test/vm/qmp.sock" ]] || \
    fail "run-vm did not clean its state lock or QMP socket"

# Merely hashing an arbitrary custom executable for the platform record is not
# authorization.  A custom QEMU must carry an independently supplied pin.
set +e
/usr/bin/env MEM=512 SMP=1 SSH_PORT=22999 QEMU_BIN="$QEMU_WRAPPER" \
    OVMF_CODE_SRC="$ovmf_code_source" OVMF_VARS_SRC="$ovmf_vars_source" \
    /usr/bin/bash "$temporary/run-vm/test/vm/run-vm.sh" \
    > "$temporary/custom.stdout" 2> "$temporary/custom.stderr"
custom_status=$?
set -e
[[ "$custom_status" -ne 0 ]] || fail "unpinned custom QEMU unexpectedly launched"
/usr/bin/grep -Fq 'requires an out-of-band EXPECTED_QEMU_SHA256' \
    "$temporary/custom.stderr" || {
        /usr/bin/cat "$temporary/custom.stdout" "$temporary/custom.stderr" >&2
        fail "unpinned custom-QEMU rejection was not explicit"
    }

printf 'PASS: canonical pinned caches, single-link artifacts, one-shot fresh epoch, locked QEMU authorization, sanitized PATH-independent launch, and inherited-FD dlopen\n'
printf 'libttsim_sha256=%s\nmanifest_sha256=%s\nbuild_input_sha256=%s\n' \
    "$library_hash" "$TTSIM_VALIDATED_MANIFEST_SHA256" "$build_input_hash"
