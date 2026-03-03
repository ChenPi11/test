#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# test_ufs.sh – Comprehensive FFS1/FFS2 driver test suite
#
# Tests every file type that OpenBSD FFS supports, based on the
# OpenBSD sys/ufs/ source:
#
#   1. Regular file     – content, size, mode, uid/gid, timestamps
#   2. Nested directory – traversal, dot/dotdot
#   3. Hard link        – same inode number, nlink count
#   4. Inline symlink   – short target stored in inode block-pointer area
#   5. File-backed symlink – long target stored in a data block
#   6. Character device – major/minor numbers preserved
#   7. Block device     – major/minor numbers preserved
#   8. FIFO             – file type correct
#   9. Unix socket      – file type correct
#  10. Setuid file       – permission bits including setuid
#
# ─────────────────────────────────────────────────────────────────────────────
# Linux UML (User Mode Linux) support
# ─────────────────────────────────────────────────────────────────────────────
# This script can run in two modes:
#
#  MODE 1 – Direct (default): loads linux_ufs.ko into the host kernel and
#            mounts the test images with a loop device.  Requires root/sudo.
#
#  MODE 2 – Linux UML: boots a User-Mode-Linux kernel that already contains
#            linux_ufs.ko, runs the tests inside the UML guest, and exits.
#            Set UML_KERNEL to the path of the UML kernel binary.
#
#            Example UML setup:
#              make ARCH=um linux_ufs.ko    # build module for UML
#              ./linux mem=256M \
#                ubd0=test_ffs1.img \
#                root=/dev/ubda init=/test_ufs_guest.sh
#
#            When UML_KERNEL is set to a readable executable, this script
#            automatically builds a minimal guest init script and launches it.
#
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRIVER_DIR="$SCRIPT_DIR"

# Paths (override via environment)
UML_KERNEL="${UML_KERNEL:-}"                    # leave empty for direct mode
MODULE="$DRIVER_DIR/linux_ufs.ko"
PYTHON="${PYTHON:-python3}"
MKIMG="$DRIVER_DIR/create_test_image.py"
MNT="${MNT:-/tmp/ufs_test_mnt}"
TMP_FFS1="${TMPDIR:-/tmp}/test_ffs1_$$.img"
TMP_FFS2="${TMPDIR:-/tmp}/test_ffs2_$$.img"

# Expected test data (must match create_test_image.py)
EXPECT_REGULAR_CONTENT="Hello FFS!"
EXPECT_NESTED_CONTENT="Nested file."
EXPECT_INLINE_TARGET="/regular.txt"
EXPECT_LONG_TARGET="/long/symlink/target/stored/in/data/block$(printf 'X%.0s' $(seq 1 84))"
EXPECT_CHARDEV_MAJOR="1"    # MKDEV(1,3) – major in hex via stat %t
EXPECT_CHARDEV_MINOR="3"
EXPECT_BLKDEV_MAJOR="8"     # MKDEV(8,0)
EXPECT_BLKDEV_MINOR="0"

# ─────────────────────────────────────────────────────────────────────────────
# Test counters and helpers
# ─────────────────────────────────────────────────────────────────────────────
PASS=0
FAIL=0
SKIP=0
CURRENT_FS=""

_color_green='\033[0;32m'
_color_red='\033[0;31m'
_color_yellow='\033[0;33m'
_color_reset='\033[0m'

ok() {
    local desc="$1"
    printf "${_color_green}PASS${_color_reset} [%s] %s\n" "$CURRENT_FS" "$desc"
    PASS=$((PASS + 1))
}

fail() {
    local desc="$1" expected="${2:-}" actual="${3:-}"
    printf "${_color_red}FAIL${_color_reset} [%s] %s" "$CURRENT_FS" "$desc"
    if [ -n "$expected" ]; then
        printf "  (expected=%q  got=%q)" "$expected" "$actual"
    fi
    printf "\n"
    FAIL=$((FAIL + 1))
}

skip() {
    local desc="$1" reason="$2"
    printf "${_color_yellow}SKIP${_color_reset} [%s] %s  (%s)\n" "$CURRENT_FS" "$desc" "$reason"
    SKIP=$((SKIP + 1))
}

# check DESCRIPTION EXPECTED ACTUAL
check() {
    local desc="$1" expected="$2" actual="$3"
    if [ "$actual" = "$expected" ]; then
        ok "$desc"
    else
        fail "$desc" "$expected" "$actual"
    fi
}

# check_contains DESCRIPTION SUBSTRING ACTUAL
check_contains() {
    local desc="$1" substr="$2" actual="$3"
    if [[ "$actual" == *"$substr"* ]]; then
        ok "$desc"
    else
        fail "$desc" "*${substr}*" "$actual"
    fi
}

# Bash file-type predicates (all use lstat semantics)
filetype() {
    local p="$1"
    if   [ -L "$p" ]; then echo "symlink"
    elif [ -d "$p" ]; then echo "directory"
    elif [ -f "$p" ]; then echo "regular"
    elif [ -c "$p" ]; then echo "chardev"
    elif [ -b "$p" ]; then echo "blkdev"
    elif [ -p "$p" ]; then echo "fifo"
    elif [ -S "$p" ]; then echo "socket"
    else echo "unknown"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Build the kernel module
# ─────────────────────────────────────────────────────────────────────────────
build_module() {
    echo "=== Building linux_ufs kernel module ==="
    make -C "$DRIVER_DIR" -j"$(nproc)" 2>&1
    if [ ! -f "$MODULE" ]; then
        echo "ERROR: Module build failed – $MODULE not found" >&2
        exit 1
    fi
    echo "Module built: $MODULE"
}

# ─────────────────────────────────────────────────────────────────────────────
# Create test images
# ─────────────────────────────────────────────────────────────────────────────
create_images() {
    echo "=== Creating test images ==="
    "$PYTHON" "$MKIMG" ffs1 "$TMP_FFS1"
    "$PYTHON" "$MKIMG" ffs2 "$TMP_FFS2"
}

# ─────────────────────────────────────────────────────────────────────────────
# Module / loop-device lifecycle
# ─────────────────────────────────────────────────────────────────────────────
LOOP_DEV=""

load_module() {
    if lsmod | grep -q '^linux_ufs'; then
        echo "Note: linux_ufs already loaded; unloading first"
        sudo rmmod linux_ufs 2>/dev/null || true
        sleep 1
    fi
    echo "Loading $MODULE"
    sudo insmod "$MODULE"
    if ! lsmod | grep -q '^linux_ufs'; then
        echo "ERROR: insmod failed" >&2
        exit 1
    fi
    echo "Module loaded."
    CURRENT_FS="setup"
    ok "module loads successfully"
}

unload_module() {
    if lsmod | grep -q '^linux_ufs'; then
        sudo rmmod linux_ufs 2>/dev/null || true
    fi
}

setup_loop() {
    local img="$1"
    LOOP_DEV="$(sudo losetup --find --show "$img")"
    echo "Loop device: $LOOP_DEV  ← $img"
}

teardown_loop() {
    if [ -n "$LOOP_DEV" ]; then
        sudo losetup -d "$LOOP_DEV" 2>/dev/null || true
        LOOP_DEV=""
    fi
}

mount_fs() {
    sudo mkdir -p "$MNT"
    if mountpoint -q "$MNT" 2>/dev/null; then
        sudo umount -l "$MNT" 2>/dev/null || true
        sleep 1
    fi
    sudo mount -t ufs2bsd -o ro "$LOOP_DEV" "$MNT"
    echo "Mounted $LOOP_DEV on $MNT"
}

umount_fs() {
    if mountpoint -q "$MNT" 2>/dev/null; then
        sudo umount "$MNT" 2>/dev/null || sudo umount -l "$MNT" 2>/dev/null || true
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# Cleanup on exit
# ─────────────────────────────────────────────────────────────────────────────
cleanup() {
    umount_fs  2>/dev/null || true
    teardown_loop 2>/dev/null || true
    unload_module 2>/dev/null || true
    rm -f "$TMP_FFS1" "$TMP_FFS2" 2>/dev/null || true
}
trap cleanup EXIT

# ─────────────────────────────────────────────────────────────────────────────
# Core test suite – run once for each mounted image
# ─────────────────────────────────────────────────────────────────────────────
run_tests() {
    local mnt="$1"

    # ── Mount / superblock ────────────────────────────────────────────────────
    check "filesystem mounted" "directory" "$(filetype "$mnt")"

    # ── Regular file ─────────────────────────────────────────────────────────
    check "regular.txt type"    "regular"   "$(filetype "$mnt/regular.txt")"
    check "regular.txt content" "$EXPECT_REGULAR_CONTENT" \
          "$(tr -d '\n' < "$mnt/regular.txt")"
    check "regular.txt size"    "11"  "$(stat -c '%s' "$mnt/regular.txt")"
    check "regular.txt mode"    "644" "$(stat -c '%a' "$mnt/regular.txt")"
    check "regular.txt uid"     "1000" "$(stat -c '%u' "$mnt/regular.txt")"
    check "regular.txt gid"     "1000" "$(stat -c '%g' "$mnt/regular.txt")"

    local reg_atime
    reg_atime="$(stat -c '%X' "$mnt/regular.txt")"
    if [ "$reg_atime" -gt 0 ] 2>/dev/null; then
        ok "regular.txt atime is non-zero"
    else
        fail "regular.txt atime is non-zero" ">0" "$reg_atime"
    fi

    local reg_mtime
    reg_mtime="$(stat -c '%Y' "$mnt/regular.txt")"
    if [ "$reg_mtime" -gt 0 ] 2>/dev/null; then
        ok "regular.txt mtime is non-zero"
    else
        fail "regular.txt mtime is non-zero" ">0" "$reg_mtime"
    fi

    # ── Nested directory ──────────────────────────────────────────────────────
    check "root is a directory"  "directory" "$(filetype "$mnt")"
    check "subdir type"          "directory" "$(filetype "$mnt/subdir")"
    check "subdir mode"          "755"       "$(stat -c '%a' "$mnt/subdir")"

    # dot entry resolves to root
    check "root . resolves to root ino" \
          "$(stat -c '%i' "$mnt")" \
          "$(stat -c '%i' "$mnt/.")"

    # dotdot in subdir resolves back to root
    check "subdir/.. ino == root ino" \
          "$(stat -c '%i' "$mnt")" \
          "$(stat -c '%i' "$mnt/subdir/..")"

    check "nested.txt type"    "regular"  "$(filetype "$mnt/subdir/nested.txt")"
    check "nested.txt content" "$EXPECT_NESTED_CONTENT" \
          "$(tr -d '\n' < "$mnt/subdir/nested.txt")"
    check "nested.txt mode"    "600"  "$(stat -c '%a' "$mnt/subdir/nested.txt")"

    # readdir: root listing contains all expected names
    local listing
    listing="$(ls "$mnt")"
    for name in regular.txt subdir inline_link long_link hardlink \
                chardev blkdev fifo socket setuid_prog; do
        check_contains "root listing contains '$name'" "$name" "$listing"
    done

    # readdir: subdir listing
    local sub_listing
    sub_listing="$(ls "$mnt/subdir")"
    check_contains "subdir listing contains 'nested.txt'" "nested.txt" "$sub_listing"

    # ── Inline symlink ────────────────────────────────────────────────────────
    check "inline_link type"     "symlink" "$(filetype "$mnt/inline_link")"
    check "inline_link target"   "$EXPECT_INLINE_TARGET" \
          "$(readlink "$mnt/inline_link")"
    # Following the symlink should yield the same content as regular.txt
    check "inline_link follows to regular.txt content" \
          "$EXPECT_REGULAR_CONTENT" \
          "$(tr -d '\n' < "$mnt/inline_link")"

    # ── File-backed (long) symlink ────────────────────────────────────────────
    check "long_link type"       "symlink" "$(filetype "$mnt/long_link")"
    local long_tgt
    long_tgt="$(readlink "$mnt/long_link")"
    check "long_link target"  "$EXPECT_LONG_TARGET" "$long_tgt"
    local long_len="${#long_tgt}"
    if [ "$long_len" -gt 60 ] 2>/dev/null; then
        ok "long_link target length ($long_len) > FFS1 inline limit (60)"
    else
        fail "long_link target length > FFS1 inline limit" ">60" "$long_len"
    fi
    if [ "$long_len" -gt 120 ] 2>/dev/null; then
        ok "long_link target length ($long_len) > FFS2 inline limit (120)"
    else
        fail "long_link target length > FFS2 inline limit" ">120" "$long_len"
    fi

    # ── Hard link ─────────────────────────────────────────────────────────────
    local reg_ino hardlink_ino
    reg_ino="$(stat -c '%i' "$mnt/regular.txt")"
    hardlink_ino="$(stat -c '%i' "$mnt/hardlink")"
    check "hardlink has same inode as regular.txt" "$reg_ino" "$hardlink_ino"

    local nlink
    nlink="$(stat -c '%h' "$mnt/regular.txt")"
    check "regular.txt nlink == 2 (has hardlink)" "2" "$nlink"

    check "hardlink content matches regular.txt" \
          "$EXPECT_REGULAR_CONTENT" \
          "$(tr -d '\n' < "$mnt/hardlink")"

    # ── Character device ──────────────────────────────────────────────────────
    check "chardev type"    "chardev" "$(filetype "$mnt/chardev")"
    check "chardev major"   "$EXPECT_CHARDEV_MAJOR" "$(stat -c '%t' "$mnt/chardev")"
    check "chardev minor"   "$EXPECT_CHARDEV_MINOR" "$(stat -c '%T' "$mnt/chardev")"
    check "chardev mode"    "644" "$(stat -c '%a' "$mnt/chardev")"

    # ── Block device ──────────────────────────────────────────────────────────
    check "blkdev type"     "blkdev"  "$(filetype "$mnt/blkdev")"
    check "blkdev major"    "$EXPECT_BLKDEV_MAJOR"  "$(stat -c '%t' "$mnt/blkdev")"
    check "blkdev minor"    "$EXPECT_BLKDEV_MINOR"  "$(stat -c '%T' "$mnt/blkdev")"
    check "blkdev mode"     "640" "$(stat -c '%a' "$mnt/blkdev")"

    # ── FIFO ──────────────────────────────────────────────────────────────────
    check "fifo type"       "fifo" "$(filetype "$mnt/fifo")"
    check "fifo mode"       "644"  "$(stat -c '%a' "$mnt/fifo")"

    # ── Unix socket ───────────────────────────────────────────────────────────
    check "socket type"     "socket" "$(filetype "$mnt/socket")"
    check "socket mode"     "755"    "$(stat -c '%a' "$mnt/socket")"

    # ── Setuid file ───────────────────────────────────────────────────────────
    check "setuid_prog type"    "regular" "$(filetype "$mnt/setuid_prog")"
    check "setuid_prog mode"    "4755"    "$(stat -c '%a' "$mnt/setuid_prog")"
    # Verify setuid bit is set via test operator
    if [ -u "$mnt/setuid_prog" ]; then
        ok "setuid_prog has S_ISUID bit set (-u test)"
    else
        fail "setuid_prog has S_ISUID bit set (-u test)"
    fi
    check "setuid_prog uid"     "0"    "$(stat -c '%u' "$mnt/setuid_prog")"
}

# ─────────────────────────────────────────────────────────────────────────────
# Linux UML mode
# ─────────────────────────────────────────────────────────────────────────────
run_in_uml() {
    # Build a minimal guest init script that loads the module and runs the
    # core tests inside the UML guest kernel.
    #
    # Prerequisites:
    #   • UML kernel compiled with ARCH=um and linux_ufs.ko for UML
    #   • A root filesystem image (e.g. a Debian base)
    #   • This script is reachable inside the guest at /test_ufs.sh
    #
    # This function documents the invocation; it requires the caller to have
    # set up the UML environment (rootfs, module, images).
    local uml_kernel="$1"
    local ffs1_img="$2"
    local ffs2_img="$3"

    echo "=== Running tests inside Linux UML kernel ==="
    echo "UML kernel: $uml_kernel"

    # Build a guest init that mirrors what this script does in direct mode.
    local guest_init
    guest_init="$(mktemp /tmp/uml_init_XXXXXX.sh)"
    cat > "$guest_init" << 'GUEST'
#!/bin/sh
# Minimal UML guest init for linux_ufs testing
mount -t proc proc /proc
mount -t sysfs sysfs /sys

insmod /linux_ufs.ko || { echo "insmod failed"; halt; }

for img in /ffs1.img /ffs2.img; do
    mkdir -p /mnt
    mount -t ufs2bsd -o ro,loop "$img" /mnt && {
        ls /mnt
        cat /mnt/regular.txt
        readlink /mnt/inline_link
        echo "--- $img OK ---"
        umount /mnt
    } || echo "mount $img FAILED"
done
halt
GUEST
    chmod +x "$guest_init"

    "$uml_kernel" mem=256M \
        ubd0="$ffs1_img" ubd1="$ffs2_img" \
        init="$guest_init" \
        con=null \
        2>&1

    rm -f "$guest_init"
}

# ─────────────────────────────────────────────────────────────────────────────
# Main entry point
# ─────────────────────────────────────────────────────────────────────────────
main() {
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║          linux_ufs  FFS1/FFS2 Comprehensive Test Suite      ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo ""

    # Step 1: Build the module
    build_module

    # Step 2: Create test images
    create_images

    # ── UML mode ──────────────────────────────────────────────────────────────
    if [ -n "$UML_KERNEL" ] && [ -x "$UML_KERNEL" ]; then
        echo ""
        echo "UML_KERNEL is set – using Linux User Mode Linux for testing"
        run_in_uml "$UML_KERNEL" "$TMP_FFS1" "$TMP_FFS2"
        echo ""
        echo "UML test run complete.  For detailed pass/fail analysis, run"
        echo "the tests in direct mode (unset UML_KERNEL)."
        exit 0
    fi

    echo ""
    echo "Running in direct mode (kernel module + loop mount)"
    echo "  (Set UML_KERNEL=/path/to/linux to use User Mode Linux instead)"
    echo ""

    # Step 3: Load module
    load_module

    # ── Test FFS1 ─────────────────────────────────────────────────────────────
    echo ""
    echo "─── FFS1 (UFS1) tests ───────────────────────────────────────────────"
    CURRENT_FS="FFS1"
    setup_loop "$TMP_FFS1"
    mount_fs
    run_tests "$MNT"
    umount_fs
    teardown_loop

    # ── Test FFS2 ─────────────────────────────────────────────────────────────
    echo ""
    echo "─── FFS2 (UFS2) tests ───────────────────────────────────────────────"
    CURRENT_FS="FFS2"
    setup_loop "$TMP_FFS2"
    mount_fs
    run_tests "$MNT"
    umount_fs
    teardown_loop

    # Step 5: Report
    echo ""
    echo "════════════════════════════════════════════════════════════════"
    echo " Test Summary"
    echo "════════════════════════════════════════════════════════════════"
    printf " Passed:  %3d\n" "$PASS"
    printf " Failed:  %3d\n" "$FAIL"
    printf " Skipped: %3d\n" "$SKIP"
    printf " Total:   %3d\n" "$((PASS + FAIL + SKIP))"
    echo "════════════════════════════════════════════════════════════════"

    if [ "$FAIL" -gt 0 ]; then
        echo " RESULT: FAIL ($FAIL test(s) failed)"
        exit 1
    else
        echo " RESULT: PASS – all $PASS tests passed"
        exit 0
    fi
}

main "$@"
