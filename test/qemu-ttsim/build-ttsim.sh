#!/usr/bin/bash
# Build the heartbeat/reset-patched Blackhole libttsim from a reviewed source
# snapshot without modifying the sibling ttsim checkout.  The only supported
# launch artifact is published under:
#   ttsim-work/artifacts/sha256/<library-sha256>/<build-input-sha256>/libttsim.so
set -euo pipefail
umask 077

# Do not let caller-provided compiler wrappers, Python modules, locale, or
# linker flags affect the source snapshot, cache identity, or output.
PATH=/usr/bin:/bin
unset CDPATH ENV BASH_ENV GLOBIGNORE PYTHONHOME PYTHONPATH PYTHONSTARTUP \
    VIRTUAL_ENV CONDA_PREFIX CC CXX CPP LD AR AS NM OBJCOPY OBJDUMP \
    RANLIB READELF STRIP CFLAGS CXXFLAGS CPPFLAGS LDFLAGS LIBRARY_PATH \
    COMPILER_PATH GCC_EXEC_PREFIX CPATH CPLUS_INCLUDE_PATH \
    LD_LIBRARY_PATH LD_PRELOAD CONFIG_SITE MAKEFLAGS MFLAGS COLOR
export PATH LC_ALL=C.UTF-8 LANG=C.UTF-8 TZ=UTC

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
cd "$SCRIPT_DIR"
BUILD_SCRIPT="$SCRIPT_DIR/build-ttsim.sh"

SRC="$SCRIPT_DIR/../../../ttsim"
WORK="$SCRIPT_DIR/ttsim-work"
HELPER="$SCRIPT_DIR/ttsim-provenance.sh"
PATCHER="$SCRIPT_DIR/apply-heartbeat-patch.py"
PATCH_TEST="$SCRIPT_DIR/test_apply_heartbeat_patch.py"

# Updating either pin is a security-review event.  The SHA-256 covers every
# regular file name and byte in `git archive` of the commit (not timestamps).
PINNED_COMMIT=4f9cffd6104d7111663f3fd644612e4ad56ef23a
PINNED_SOURCE_TREE_SHA256=b272956d08dea773434ddc5e91620b81e9715adf65663ed07d114e91a28af508

[[ -f "$HELPER" && ! -L "$HELPER" ]] || {
    echo "missing regular provenance helper: $HELPER" >&2
    exit 1
}
# Pin and hash the helper before sourcing it.  Sourcing by pathname and hashing
# later could attribute functions already executed from one inode to another.
exec {HELPER_INPUT_FD}<"$HELPER"
helper_path_identity=$(/usr/bin/stat -Lc '%d:%i:%s:%u:%a:%h' -- "$HELPER")
helper_open_identity=$(/usr/bin/stat -Lc '%d:%i:%s:%u:%a:%h' -- "/proc/$$/fd/$HELPER_INPUT_FD")
IFS=: read -r _ _ _ helper_uid helper_mode helper_links <<< "$helper_open_identity"
[[ "$helper_path_identity" == "$helper_open_identity" &&
   "$helper_uid" == "$(/usr/bin/id -u)" && "$helper_links" == 1 &&
   $((8#$helper_mode & 0022)) -eq 0 ]] || {
    echo "provenance helper inode is unstable or writable by group/other" >&2
    exit 1
}
helper_sha256=$(/usr/bin/sha256sum "/proc/$$/fd/$HELPER_INPUT_FD" | /usr/bin/cut -d ' ' -f1)
# shellcheck source=ttsim-provenance.sh
source "/proc/$$/fd/$HELPER_INPUT_FD"
[[ "$(/usr/bin/sha256sum "/proc/$$/fd/$HELPER_INPUT_FD" | /usr/bin/cut -d ' ' -f1)" == "$helper_sha256" ]] || {
    echo "provenance helper changed while it was sourced" >&2
    exit 1
}

die() {
    printf 'build-ttsim: %s\n' "$*" >&2
    exit 1
}

ensure_plain_directory() {
    local directory=$1
    if [[ -e "$directory" || -L "$directory" ]]; then
        [[ -d "$directory" && ! -L "$directory" ]] || \
            die "refusing non-directory or symlinked path: $directory"
    else
        /usr/bin/mkdir -- "$directory"
    fi
}

validate_owned_directory() {
    local directory=$1 owner mode expected_uid
    expected_uid=$(/usr/bin/id -u) || return 1
    IFS=: read -r owner mode < <(/usr/bin/stat -Lc '%u:%a' -- "$directory") || return 1
    [[ "$owner" == "$expected_uid" ]] || \
        die "generated directory is not owned by the invoking user: $directory"
    (( (8#$mode & 0022) == 0 )) || \
        die "generated directory is group/other writable: $directory (mode $mode)"
}

safe_remove_scratch() {
    local target=$1 resolved root
    [[ ! -L "$target" ]] || die "refusing symlinked scratch path: $target"
    resolved=$(/usr/bin/realpath -m -- "$target")
    root=$(/usr/bin/realpath -e -- "$WORK")
    case "$resolved" in
        "$root"/.build.*) /usr/bin/rm -rf -- "$resolved" ;;
        *) die "refusing scratch cleanup outside generated namespace: $resolved" ;;
    esac
}

for file in "$PATCHER" "$PATCH_TEST" "$BUILD_SCRIPT"; do
    [[ -f "$file" && ! -L "$file" ]] || die "required input is missing or redirected: $file"
done
exec {PATCHER_INPUT_FD}<"$PATCHER"
exec {PATCH_TEST_INPUT_FD}<"$PATCH_TEST"
patcher_path_identity=$(/usr/bin/stat -Lc '%d:%i:%s:%u:%a:%h' -- "$PATCHER")
patcher_open_identity=$(/usr/bin/stat -Lc '%d:%i:%s:%u:%a:%h' -- "/proc/$$/fd/$PATCHER_INPUT_FD")
patch_test_path_identity=$(/usr/bin/stat -Lc '%d:%i:%s:%u:%a:%h' -- "$PATCH_TEST")
patch_test_open_identity=$(/usr/bin/stat -Lc '%d:%i:%s:%u:%a:%h' -- "/proc/$$/fd/$PATCH_TEST_INPUT_FD")
for input_record in "$patcher_open_identity" "$patch_test_open_identity"; do
    IFS=: read -r _ _ _ input_uid input_mode input_links <<< "$input_record"
    [[ "$input_uid" == "$(/usr/bin/id -u)" && "$input_links" == 1 &&
       $((8#$input_mode & 0022)) -eq 0 ]] || \
        die "patch input is not owner-owned, single-link, and non-writable by group/other"
done
[[ "$patcher_path_identity" == "$patcher_open_identity" &&
   "$patch_test_path_identity" == "$patch_test_open_identity" ]] || \
    die "patch input pathname changed while opening"
patcher_sha256=$(/usr/bin/sha256sum "/proc/$$/fd/$PATCHER_INPUT_FD" | /usr/bin/cut -d ' ' -f1)
patch_test_sha256=$(/usr/bin/sha256sum "/proc/$$/fd/$PATCH_TEST_INPUT_FD" | /usr/bin/cut -d ' ' -f1)
build_script_sha256=$(ttsim_sha256_file "$BUILD_SCRIPT")

verify_build_inputs() {
    [[ "$(/usr/bin/stat -Lc '%d:%i:%s:%u:%a:%h' -- "$HELPER")" == "$helper_open_identity" &&
       "$(/usr/bin/stat -Lc '%d:%i:%s:%u:%a:%h' -- "$PATCHER")" == "$patcher_open_identity" &&
       "$(/usr/bin/stat -Lc '%d:%i:%s:%u:%a:%h' -- "$PATCH_TEST")" == "$patch_test_open_identity" &&
       "$(/usr/bin/sha256sum "/proc/$$/fd/$HELPER_INPUT_FD" | /usr/bin/cut -d ' ' -f1)" == "$helper_sha256" &&
       "$(/usr/bin/sha256sum "/proc/$$/fd/$PATCHER_INPUT_FD" | /usr/bin/cut -d ' ' -f1)" == "$patcher_sha256" &&
       "$(/usr/bin/sha256sum "/proc/$$/fd/$PATCH_TEST_INPUT_FD" | /usr/bin/cut -d ' ' -f1)" == "$patch_test_sha256" &&
       "$(ttsim_sha256_file "$BUILD_SCRIPT")" == "$build_script_sha256" ]] || \
        die "build helper/patch/test/script changed after input capture"
}
verify_build_inputs
[[ -d "$SRC" && ! -L "$SRC" ]] || die "ttsim source root is missing or redirected: $SRC"
src_root=$(/usr/bin/git -C "$SRC" rev-parse --show-toplevel 2>/dev/null) || \
    die "ttsim source is not a Git checkout: $SRC"
[[ "$(/usr/bin/realpath -e -- "$src_root")" == "$(/usr/bin/realpath -e -- "$SRC")" ]] || \
    die "ttsim Git root does not match the expected sibling checkout"

actual_commit=$(/usr/bin/git -C "$SRC" rev-parse --verify 'HEAD^{commit}') || \
    die "cannot resolve ttsim HEAD"
[[ "$actual_commit" =~ ^[0-9a-f]{40}$ ]] || die "malformed ttsim commit: $actual_commit"
# The checkout is on DrvFS: ignore its synthetic executable bit without
# changing the sibling repository's core.filemode setting.
source_status=$(/usr/bin/git -c core.filemode=false -C "$SRC" \
    status --porcelain=v1 --untracked-files=all) || \
    die "cannot inspect ttsim checkout state"
if [[ -n "$source_status" ]]; then
    source_state=dirty
else
    source_state=clean
fi

reviewed_override=${TTSIM_REVIEWED_OVERRIDE:-0}
case "$reviewed_override" in
    0)
        [[ -z "${TTSIM_REVIEWED_COMMIT:-}" &&
           -z "${TTSIM_REVIEWED_SOURCE_TREE_SHA256:-}" ]] || \
            die "reviewed commit/hash were supplied without TTSIM_REVIEWED_OVERRIDE=1"
        [[ "$actual_commit" == "$PINNED_COMMIT" ]] || \
            die "ttsim HEAD $actual_commit is not pinned $PINNED_COMMIT"
        [[ "$source_state" == clean ]] || {
            printf '%s\n' "$source_status" >&2
            die "default builds require a completely clean pinned ttsim checkout"
        }
        source_mode=pinned-clean
        expected_source_hash=$PINNED_SOURCE_TREE_SHA256
        ;;
    1)
        reviewed_commit=${TTSIM_REVIEWED_COMMIT:-}
        reviewed_hash=${TTSIM_REVIEWED_SOURCE_TREE_SHA256:-}
        [[ "$reviewed_commit" =~ ^[0-9a-f]{40}$ &&
           "$reviewed_hash" =~ ^[0-9a-f]{64}$ ]] || \
            die "override requires exact TTSIM_REVIEWED_COMMIT and TTSIM_REVIEWED_SOURCE_TREE_SHA256"
        [[ "$actual_commit" == "$reviewed_commit" ]] || \
            die "reviewed commit $reviewed_commit does not match checkout HEAD $actual_commit"
        source_mode=reviewed-override
        expected_source_hash=$reviewed_hash
        ;;
    *) die "TTSIM_REVIEWED_OVERRIDE must be exactly 0 or 1" ;;
esac

ensure_plain_directory "$WORK"
validate_owned_directory "$WORK"
ARTIFACT_ROOT="$WORK/artifacts"
ARTIFACT_SHA_ROOT="$ARTIFACT_ROOT/sha256"
CACHE_ROOT="$WORK/cache"
for directory in "$ARTIFACT_ROOT" "$ARTIFACT_SHA_ROOT" "$CACHE_ROOT"; do
    ensure_plain_directory "$directory"
    validate_owned_directory "$directory"
done

BUILD_LOCK="$WORK/.build.lock"
PUBLICATION_LOCK="$ARTIFACT_ROOT/.publication.lock"
for lock in "$BUILD_LOCK" "$PUBLICATION_LOCK"; do
    [[ ! -L "$lock" && ( ! -e "$lock" || -f "$lock" ) ]] || \
        die "refusing non-regular or redirected lock: $lock"
done
exec 9>>"$BUILD_LOCK"
/usr/bin/flock -x 9
ttsim_validate_open_lock "$BUILD_LOCK" 9 || exit 1
exec 8>>"$PUBLICATION_LOCK"
/usr/bin/flock -x 8
ttsim_validate_open_lock "$PUBLICATION_LOCK" 8 || exit 1
/usr/bin/flock -u 8

SCRATCH=$(/usr/bin/mktemp -d "$WORK/.build.XXXXXXXX")
publish_stage=''
cleanup() {
    local status=$?
    trap - EXIT
    if [[ -n "${SCRATCH:-}" && -d "$SCRATCH" ]]; then
        safe_remove_scratch "$SCRATCH" || status=1
    fi
    if [[ -n "${publish_stage:-}" && -d "$publish_stage" &&
          ! -L "$publish_stage" ]]; then
        resolved_publish=$(/usr/bin/realpath -m -- "$publish_stage")
        resolved_sha_root=$(/usr/bin/realpath -e -- "$ARTIFACT_SHA_ROOT")
        case "$resolved_publish" in
            "$resolved_sha_root"/.publish.*)
                /usr/bin/chmod 0700 -- "$resolved_publish" || status=1
                /usr/bin/rm -rf -- "$resolved_publish" || status=1
                ;;
            *) status=1 ;;
        esac
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

SOURCE_SNAPSHOT="$SCRATCH/source"
/usr/bin/mkdir -- "$SOURCE_SNAPSHOT"
INPUT_SNAPSHOT="$SCRATCH/inputs"
/usr/bin/mkdir -- "$INPUT_SNAPSHOT"
/usr/bin/install -m 0400 -- "/proc/$$/fd/$PATCHER_INPUT_FD" \
    "$INPUT_SNAPSHOT/apply-heartbeat-patch.py"
/usr/bin/install -m 0400 -- "/proc/$$/fd/$PATCH_TEST_INPUT_FD" \
    "$INPUT_SNAPSHOT/test_apply_heartbeat_patch.py"
PATCHER_SNAPSHOT="$INPUT_SNAPSHOT/apply-heartbeat-patch.py"
PATCH_TEST_SNAPSHOT="$INPUT_SNAPSHOT/test_apply_heartbeat_patch.py"
[[ "$(ttsim_sha256_file "$PATCHER_SNAPSHOT")" == "$patcher_sha256" &&
   "$(ttsim_sha256_file "$PATCH_TEST_SNAPSHOT")" == "$patch_test_sha256" ]] || \
    die "private patch-input snapshot changed while copying"
if [[ "$source_mode" == pinned-clean ]]; then
    /usr/bin/git -C "$SRC" archive --format=tar "$actual_commit" |
        /usr/bin/tar -C "$SOURCE_SNAPSHOT" -xf -
else
    unexpected=$(
        cd "$SRC"
        /usr/bin/find . \
            -path './.git' -prune -o \
            -path './src/_out' -prune -o \
            \( -type l -o \( ! -type d ! -type f \) \) -print -quit
    )
    [[ -z "$unexpected" ]] || die "reviewed source contains a link or special entry: $unexpected"
    /usr/bin/rsync -a --delete --exclude '/.git' --exclude '/src/_out' \
        "$SRC/" "$SOURCE_SNAPSHOT/"
fi

source_tree_sha256=$(ttsim_hash_regular_tree "$SOURCE_SNAPSHOT") || exit 1
[[ "$source_tree_sha256" == "$expected_source_hash" ]] || \
    die "source tree SHA256 $source_tree_sha256 is not reviewed $expected_source_hash"

# A concurrent checkout operation must not silently change the provenance
# label.  Override builds remain bound to the already-copied exact snapshot.
final_commit=$(/usr/bin/git -C "$SRC" rev-parse --verify 'HEAD^{commit}') || \
    die "ttsim checkout disappeared while snapshotting"
[[ "$final_commit" == "$actual_commit" ]] || \
    die "ttsim HEAD changed while the source snapshot was captured"
if [[ "$source_mode" == pinned-clean ]]; then
    final_status=$(/usr/bin/git -c core.filemode=false -C "$SRC" \
        status --porcelain=v1 --untracked-files=all) || \
        die "cannot recheck ttsim checkout state"
    [[ -z "$final_status" ]] || die "ttsim checkout changed while the pinned snapshot was captured"
fi

# Unit-test the anchor-based transformation before using it as a build input.
/usr/bin/env -i PATH=/usr/bin:/bin LC_ALL=C.UTF-8 LANG=C.UTF-8 TZ=UTC \
    PYTHONDONTWRITEBYTECODE=1 TTSIM_TEST_PATCHER="$PATCHER_SNAPSHOT" \
    TTSIM_TEST_SOURCE_DIR="$SOURCE_SNAPSHOT/src" \
    /usr/bin/python3 -B "$PATCH_TEST_SNAPSHOT" -v
/usr/bin/env -i PATH=/usr/bin:/bin LC_ALL=C.UTF-8 LANG=C.UTF-8 TZ=UTC \
    PYTHONDONTWRITEBYTECODE=1 \
    /usr/bin/python3 -B "$PATCHER_SNAPSHOT" "$SOURCE_SNAPSHOT/src/tile.cpp"
patched_tree_sha256=$(ttsim_hash_regular_tree "$SOURCE_SNAPSHOT") || exit 1
verify_build_inputs

toolchain_sha256="$({
    printf '%s\n' 'toolchain-schema=tt-win-kmd-ttsim-toolchain-v1'
    for tool in /usr/bin/bash /usr/bin/env /usr/bin/python3 /usr/bin/g++ \
            /usr/bin/as /usr/bin/ld /usr/bin/nm /usr/bin/readelf \
            /usr/bin/strip /usr/bin/sha256sum /usr/bin/git /usr/bin/tar \
            /usr/bin/rsync /usr/bin/find /usr/bin/sort /usr/bin/cut \
            /usr/bin/tr /usr/bin/awk /usr/bin/diff /usr/bin/flock \
            /usr/bin/realpath /usr/bin/stat /usr/bin/id /usr/bin/install /usr/bin/cp \
            /usr/bin/mv /usr/bin/chmod /usr/bin/mkdir /usr/bin/mktemp \
            /usr/bin/touch /usr/bin/sync /usr/bin/rm; do
        resolved=$(/usr/bin/realpath -e -- "$tool") || exit 1
        [[ -f "$resolved" ]] || die "tool does not resolve to a regular file: $tool"
        printf 'tool=%s resolved=%s sha256=' "$tool" "$resolved"
        /usr/bin/sha256sum -- "$resolved" | /usr/bin/cut -d ' ' -f1
    done
    for compiler_program in cc1plus collect2 lto-wrapper as ld; do
        program=$(/usr/bin/g++ -print-prog-name="$compiler_program") || exit 1
        if [[ "$program" != */* ]]; then
            program=$(type -P -- "$program") || exit 1
        fi
        resolved=$(/usr/bin/realpath -e -- "$program") || exit 1
        printf 'compiler_program=%s resolved=%s sha256=' "$compiler_program" "$resolved"
        /usr/bin/sha256sum -- "$resolved" | /usr/bin/cut -d ' ' -f1
    done
    for compiler_file in libstdc++.so libgcc_s.so crtbeginS.o crti.o; do
        path=$(/usr/bin/g++ -print-file-name="$compiler_file") || exit 1
        resolved=$(/usr/bin/realpath -e -- "$path") || exit 1
        printf 'compiler_file=%s resolved=%s sha256=' "$compiler_file" "$resolved"
        /usr/bin/sha256sum -- "$resolved" | /usr/bin/cut -d ' ' -f1
    done
    /usr/bin/g++ --version
    /usr/bin/g++ -dumpmachine
    /usr/bin/g++ -dumpfullversion -dumpversion
    /usr/bin/g++ -print-search-dirs
    /usr/bin/g++ -dumpspecs
    if [[ -f /var/lib/dpkg/status && ! -L /var/lib/dpkg/status ]]; then
        printf 'dpkg_status_sha256='
        /usr/bin/sha256sum /var/lib/dpkg/status | /usr/bin/cut -d ' ' -f1
    fi
} | /usr/bin/sha256sum | /usr/bin/cut -d ' ' -f1)"
[[ "$toolchain_sha256" =~ ^[0-9a-f]{64}$ ]] || die "failed to bind toolchain identity"

build_env_sha256=$(
    printf '%s\n' \
        'PATH=/usr/bin:/bin' 'HOME=<private-build-home>' \
        'TMPDIR=<private-build-tmp>' 'USER=nobody' 'SHELL=/bin/sh' \
        'LC_ALL=C.UTF-8' 'LANG=C.UTF-8' 'TZ=UTC' \
        'SOURCE_DATE_EPOCH=0' 'PYTHONDONTWRITEBYTECODE=1' |
        /usr/bin/sha256sum | /usr/bin/cut -d ' ' -f1
)
exports_sha256=$(ttsim_exports_sha256)
build_input_sha256=$(
    printf '%s\n' \
        "source_commit=$actual_commit" \
        "source_tree_sha256=$source_tree_sha256" \
        "source_mode=$source_mode" "source_state=$source_state" \
        "patched_tree_sha256=$patched_tree_sha256" \
        "patcher_sha256=$patcher_sha256" \
        "patch_test_sha256=$patch_test_sha256" \
        "build_script_sha256=$build_script_sha256" \
        "helper_sha256=$helper_sha256" \
        "toolchain_sha256=$toolchain_sha256" \
        "build_env_sha256=$build_env_sha256" \
        "exports_sha256=$exports_sha256" \
        'target=src/_out/release_bh/libttsim.so' |
        /usr/bin/sha256sum | /usr/bin/cut -d ' ' -f1
)

validate_current_artifact() {
    local manifest=$1
    ttsim_validate_artifact "$manifest" || return 1
    [[ ${TTSIM_MANIFEST[source_commit]} == "$actual_commit" &&
       ${TTSIM_MANIFEST[source_tree_sha256]} == "$source_tree_sha256" &&
       ${TTSIM_MANIFEST[source_mode]} == "$source_mode" &&
       ${TTSIM_MANIFEST[source_state]} == "$source_state" &&
       ${TTSIM_MANIFEST[patched_tree_sha256]} == "$patched_tree_sha256" &&
       ${TTSIM_MANIFEST[patcher_sha256]} == "$patcher_sha256" &&
       ${TTSIM_MANIFEST[patch_test_sha256]} == "$patch_test_sha256" &&
       ${TTSIM_MANIFEST[build_script_sha256]} == "$build_script_sha256" &&
       ${TTSIM_MANIFEST[helper_sha256]} == "$helper_sha256" &&
       ${TTSIM_MANIFEST[toolchain_sha256]} == "$toolchain_sha256" &&
       ${TTSIM_MANIFEST[build_env_sha256]} == "$build_env_sha256" &&
       ${TTSIM_MANIFEST[build_input_sha256]} == "$build_input_sha256" &&
       ${TTSIM_MANIFEST[exports_sha256]} == "$exports_sha256" ]]
}

remove_exact_invalid_artifact() {
    local directory=$1 expected=$2 resolved owner
    [[ -d "$directory" && ! -L "$directory" ]] || return 0
    resolved=$(/usr/bin/realpath -m -- "$directory") || return 1
    [[ "$resolved" == "$expected" ]] || \
        die "refusing artifact recovery outside exact addressed path: $resolved"
    owner=$(/usr/bin/stat -Lc '%u' -- "$resolved") || return 1
    [[ "$owner" == "$(/usr/bin/id -u)" ]] || \
        die "refusing recovery of artifact owned by another user: $resolved"
    /usr/bin/chmod 0700 -- "$resolved"
    /usr/bin/rm -rf -- "$resolved"
}

CACHE_REF="$CACHE_ROOT/$build_input_sha256.ref"
[[ ! -L "$CACHE_REF" && ( ! -e "$CACHE_REF" || -f "$CACHE_REF" ) ]] || \
    die "refusing non-regular or redirected cache reference: $CACHE_REF"
cache_hit=0
if [[ -f "$CACHE_REF" ]]; then
    /usr/bin/flock -s 8
    ttsim_validate_open_lock "$PUBLICATION_LOCK" 8 || exit 1
    IFS=: read -r ref_mode ref_uid ref_links < <(
        /usr/bin/stat -Lc '%a:%u:%h' -- "$CACHE_REF"
    )
    [[ "$ref_mode" == 444 && "$ref_uid" == "$(/usr/bin/id -u)" &&
       "$ref_links" == 1 ]] || \
        die "cache reference must be owner-owned, single-link mode 0444: $CACHE_REF"
    mapfile -t ref_lines < "$CACHE_REF"
    if [[ ${#ref_lines[@]} -eq 1 && ${ref_lines[0]} =~ ^[0-9a-f]{64}$ ]]; then
        cached_library_sha256=${ref_lines[0]}
        cached_artifact="$ARTIFACT_SHA_ROOT/$cached_library_sha256/$build_input_sha256"
        cached_manifest="$cached_artifact/$TTSIM_MANIFEST_NAME"
        if validate_current_artifact "$cached_manifest"; then
            cache_hit=1
        fi
    fi
    /usr/bin/flock -u 8
    if [[ "$cache_hit" -eq 0 ]]; then
        # A validly owned immutable ref can survive a power-loss/interrupted
        # publication while its addressed artifact is absent or incomplete.
        # Recheck under exclusive publication lock, remove only that exact ref
        # and addressed directory, then rebuild instead of wedging forever.
        /usr/bin/flock -x 8
        ttsim_validate_open_lock "$PUBLICATION_LOCK" 8 || exit 1
        IFS=: read -r ref_mode ref_uid ref_links < <(
            /usr/bin/stat -Lc '%a:%u:%h' -- "$CACHE_REF"
        )
        [[ "$ref_mode" == 444 && "$ref_uid" == "$(/usr/bin/id -u)" &&
           "$ref_links" == 1 && ! -L "$CACHE_REF" ]] || \
            die "invalid cache ref changed during recovery: $CACHE_REF"
        if [[ -n "${cached_artifact:-}" ]]; then
            remove_exact_invalid_artifact "$cached_artifact" "$cached_artifact"
        fi
        /usr/bin/rm -f -- "$CACHE_REF"
        /usr/bin/sync -f "$CACHE_ROOT"
        /usr/bin/flock -u 8
    fi
fi
if [[ "$cache_hit" -eq 0 ]]; then
    BUILD_HOME="$SCRATCH/home"
    BUILD_TMP="$SCRATCH/tmp"
    /usr/bin/mkdir -- "$BUILD_HOME" "$BUILD_TMP"
    (
        cd "$SOURCE_SNAPSHOT"
        /usr/bin/env -i \
            PATH=/usr/bin:/bin HOME="$BUILD_HOME" TMPDIR="$BUILD_TMP" \
            USER=nobody SHELL=/bin/sh LC_ALL=C.UTF-8 LANG=C.UTF-8 TZ=UTC \
            SOURCE_DATE_EPOCH=0 PYTHONDONTWRITEBYTECODE=1 \
            /usr/bin/python3 -B ./make.py --inherit-env \
                src/_out/release_bh/libttsim.so
    )
    built_library="$SOURCE_SNAPSHOT/src/_out/release_bh/libttsim.so"
    [[ -f "$built_library" && ! -L "$built_library" ]] || \
        die "build did not produce a regular libttsim.so"
    ttsim_validate_elf "$built_library" || exit 1
    library_sha256=$(ttsim_sha256_file "$built_library")
    library_size=$(/usr/bin/stat -c %s -- "$built_library")
    artifact_hash_directory="$ARTIFACT_SHA_ROOT/$library_sha256"
    artifact_directory="$artifact_hash_directory/$build_input_sha256"
    publish_stage=$(/usr/bin/mktemp -d "$ARTIFACT_SHA_ROOT/.publish.XXXXXXXX")
    /usr/bin/install -m 0444 -- "$built_library" "$publish_stage/libttsim.so"
    manifest_tmp="$publish_stage/.manifest.XXXXXXXX"
    printf '%s\n' \
        "schema=$TTSIM_MANIFEST_SCHEMA" \
        'library=libttsim.so' \
        "library_sha256=$library_sha256" \
        "library_size=$library_size" \
        "source_commit=$actual_commit" \
        "source_tree_sha256=$source_tree_sha256" \
        "source_mode=$source_mode" "source_state=$source_state" \
        "patched_tree_sha256=$patched_tree_sha256" \
        "patcher_sha256=$patcher_sha256" \
        "patch_test_sha256=$patch_test_sha256" \
        "build_script_sha256=$build_script_sha256" \
        "helper_sha256=$helper_sha256" \
        "toolchain_sha256=$toolchain_sha256" \
        "build_env_sha256=$build_env_sha256" \
        "build_input_sha256=$build_input_sha256" \
        "exports_sha256=$exports_sha256" > "$manifest_tmp"
    /usr/bin/chmod 0444 -- "$manifest_tmp"
    /usr/bin/mv -T -- "$manifest_tmp" "$publish_stage/$TTSIM_MANIFEST_NAME"
    /usr/bin/sync -f "$publish_stage/libttsim.so"
    /usr/bin/sync -f "$publish_stage/$TTSIM_MANIFEST_NAME"
    /usr/bin/sync -f "$publish_stage"

    /usr/bin/flock -x 8
    ttsim_validate_open_lock "$PUBLICATION_LOCK" 8 || exit 1
    if [[ -e "$artifact_hash_directory" || -L "$artifact_hash_directory" ]]; then
        [[ -d "$artifact_hash_directory" && ! -L "$artifact_hash_directory" ]] || \
            die "content-addressed hash path is not a plain directory: $artifact_hash_directory"
    else
        /usr/bin/mkdir -- "$artifact_hash_directory"
        /usr/bin/chmod 0755 -- "$artifact_hash_directory"
    fi
    if [[ -e "$artifact_directory" || -L "$artifact_directory" ]]; then
        [[ -d "$artifact_directory" && ! -L "$artifact_directory" ]] || \
            die "content-addressed artifact path is not a plain directory: $artifact_directory"
        if validate_current_artifact "$artifact_directory/$TTSIM_MANIFEST_NAME"; then
            /usr/bin/rm -rf -- "$publish_stage"
            publish_stage=''
        else
            remove_exact_invalid_artifact "$artifact_directory" "$artifact_directory"
            /usr/bin/mv -- "$publish_stage" "$artifact_directory"
            publish_stage=''
            /usr/bin/chmod 0555 -- "$artifact_directory"
        fi
    else
        /usr/bin/mv -- "$publish_stage" "$artifact_directory"
        publish_stage=''
        /usr/bin/chmod 0555 -- "$artifact_directory"
        if ! validate_current_artifact "$artifact_directory/$TTSIM_MANIFEST_NAME"; then
            resolved_artifact=$(/usr/bin/realpath -m -- "$artifact_directory")
            expected_artifact="$ARTIFACT_SHA_ROOT/$library_sha256/$build_input_sha256"
            if [[ "$resolved_artifact" == "$expected_artifact" ]]; then
                /usr/bin/chmod 0700 -- "$resolved_artifact" || true
                /usr/bin/rm -rf -- "$resolved_artifact" || true
            fi
            die "atomically published artifact failed post-publication validation"
        fi
    fi
    validate_current_artifact "$artifact_directory/$TTSIM_MANIFEST_NAME" || \
        die "published/recovered content-addressed artifact failed validation"
    verify_build_inputs
    /usr/bin/sync -f "$artifact_hash_directory"

    ref_tmp=$(/usr/bin/mktemp "$CACHE_ROOT/.ref.XXXXXXXX")
    printf '%s\n' "$library_sha256" > "$ref_tmp"
    /usr/bin/chmod 0444 -- "$ref_tmp"
    /usr/bin/sync -f "$ref_tmp"
    /usr/bin/mv -fT -- "$ref_tmp" "$CACHE_REF"
    /usr/bin/sync -f "$CACHE_ROOT"
    IFS=: read -r ref_mode ref_uid ref_links < <(
        /usr/bin/stat -Lc '%a:%u:%h' -- "$CACHE_REF"
    )
    [[ "$ref_mode" == 444 && "$ref_uid" == "$(/usr/bin/id -u)" &&
       "$ref_links" == 1 ]] || \
        die "published cache reference failed ownership/link/mode validation"
    /usr/bin/flock -u 8
fi

manifest=$TTSIM_VALIDATED_MANIFEST
library=$TTSIM_VALIDATED_LIBRARY
library_hash=$TTSIM_VALIDATED_SHA256
manifest_hash=$TTSIM_VALIDATED_MANIFEST_SHA256
printf '\n'
if [[ "$cache_hit" -eq 1 ]]; then
    printf 'Reused exact completed ttsim artifact.\n'
else
    printf 'Built and atomically published exact ttsim artifact.\n'
fi
printf 'Source: commit=%s state=%s mode=%s tree_sha256=%s\n' \
    "$actual_commit" "$source_state" "$source_mode" "$source_tree_sha256"
printf 'Build input SHA256: %s\nLibrary SHA256: %s\nManifest SHA256: %s\n' \
    "$build_input_sha256" "$library_hash" "$manifest_hash"
printf 'TTSIM_LIB=%s\nTTSIM_MANIFEST=%s\n' "$library" "$manifest"
