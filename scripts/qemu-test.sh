#!/usr/bin/env bash
# qemu-test.sh: boot owos.bin headless with SLIRP networking, drive the COM1
# recovery console with a scripted sequence of commands, and capture the serial
# log. Meant for automated, non-interactive verification (TLS fetches, etc.).
# With --arch aarch64 it boots owos-aarch64.bin on QEMU virt instead, diskless
# (the system runs from its RAM filesystem) and without a NIC.
#
# It always runs against a THROWAWAY COPY of the disk image, so it never takes
# the write-lock on (or mutates) the real disk.img; a concurrent `make run` is
# fine. gtimeout hard-caps the run so a hang can't wedge the harness.
#
# Usage:
#   scripts/qemu-test.sh [options] -- "cmd1" "cmd2" ...
#
# Each "cmd" is typed into the recovery console (over COM1) in order, one line
# at a time, with --gap seconds between them. Example:
#   scripts/qemu-test.sh -- "curl -i https://www.youtube.com/ -o /home/root/y" \
#                           "ls -l /home/root"
#
# Options (flag or env var):
#   --arch NAME    i386 | aarch64                                    (ARCH,      default i386)
#   --boot-wait N  seconds to wait for boot + DHCP lease before cmd 1 (BOOT_WAIT, default 16)
#   --gap N        seconds between commands                          (GAP,       default 4)
#   --drain N      seconds to keep capturing after the last command  (DRAIN,     default 10)
#   --nic NAME     rtl8139 | e1000 | igb | vmxnet3                           (NIC,       default rtl8139)
#   --no-net       boot without a NIC
#   --smp N        cores                                             (SMP,       default 4)
#   --kernel PATH  kernel image                    (KERNEL, default owos.bin, owos-aarch64.bin for aarch64)
#   --disk PATH    disk image to copy from                           (DISK,      default disk.img)
#   --log PATH     serial capture file                               (LOG,       default /tmp/owos-test.log)
#   --timeout N    hard cap in seconds (default: boot-wait + gap*ncmds + drain + 6)
#
# On exit it prints the captured serial log to stdout and leaves it at --log.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BOOT_WAIT="${BOOT_WAIT:-16}"; GAP="${GAP:-4}"; DRAIN="${DRAIN:-10}"
NIC="${NIC:-rtl8139}"; SMP="${SMP:-4}"; NET=1
KERNEL="${KERNEL:-}"; DISK="${DISK:-disk.img}"; LOG="${LOG:-/tmp/owos-test.log}"
ARCH="${ARCH:-i386}"
ACCEL="${ACCEL:-hvf:tcg}"   # hvf on macOS w/ virt, falls back to tcg; set ACCEL=tcg to silence
TIMEOUT=""

CMDS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --arch) ARCH="$2"; shift 2;;
    --boot-wait) BOOT_WAIT="$2"; shift 2;;
    --gap) GAP="$2"; shift 2;;
    --drain) DRAIN="$2"; shift 2;;
    --nic) NIC="$2"; shift 2;;
    --no-net) NET=0; shift;;
    --smp) SMP="$2"; shift 2;;
    --accel) ACCEL="$2"; shift 2;;
    --kernel) KERNEL="$2"; shift 2;;
    --disk) DISK="$2"; shift 2;;
    --log) LOG="$2"; shift 2;;
    --timeout) TIMEOUT="$2"; shift 2;;
    --) shift; CMDS=("$@"); break;;
    *) echo "qemu-test: unknown arg '$1'" >&2; exit 2;;
  esac
done

case "$ARCH" in
  i386) KERNEL="${KERNEL:-owos.bin}";;
  aarch64) KERNEL="${KERNEL:-owos-aarch64.bin}"; NET=0;;
  *) echo "qemu-test: unknown arch '$ARCH'" >&2; exit 2;;
esac

[ -f "$KERNEL" ] || { echo "qemu-test: kernel '$KERNEL' not found (run make?)" >&2; exit 1; }
[ "$ARCH" = aarch64 ] || [ -f "$DISK" ] || { echo "qemu-test: disk '$DISK' not found" >&2; exit 1; }

# gtimeout on macOS (brew coreutils); timeout elsewhere.
TO="$(command -v gtimeout || command -v timeout || true)"
[ -n "$TO" ] || { echo "qemu-test: need gtimeout/timeout (brew install coreutils)" >&2; exit 1; }

if [ "$ARCH" = aarch64 ]; then QEMU="${QEMU:-qemu-system-aarch64}"; else QEMU="${QEMU:-qemu-system-i386}"; fi
command -v "$QEMU" >/dev/null || { echo "qemu-test: '$QEMU' not found" >&2; exit 1; }

if [ -z "$TIMEOUT" ]; then
  TIMEOUT=$(( BOOT_WAIT + GAP * ${#CMDS[@]} + DRAIN + 6 ))
fi

MACHINE=()
if [ "$ARCH" = aarch64 ]; then
  MACHINE=(-M virt,virtualization=on,gic-version="${A64_GIC:-2}" -cpu cortex-a57)
else
  TMPDISK="$(mktemp -t owos-test.XXXXXX.img)"
  cleanup() { rm -f "$TMPDISK"; }
  trap cleanup EXIT
  cp "$DISK" "$TMPDISK"
  MACHINE=(-drive file="$TMPDISK",format=raw,if=ide -machine accel="$ACCEL")
fi

NETARGS=()
[ "$NET" = 1 ] && NETARGS=(-netdev user,id=net0 -device "$NIC",netdev=net0)

# Feed the scripted commands into COM1 (guest stdin) with paced delays.
# `${CMDS[@]+...}` guards the empty-array-under-set-u error on bash 3.2 (macOS).
feed() {
  sleep "$BOOT_WAIT"
  for c in ${CMDS[@]+"${CMDS[@]}"}; do printf '%s\n' "$c"; sleep "$GAP"; done
  sleep "$DRAIN"
}

echo "qemu-test: boot=$BOOT_WAIT gap=$GAP drain=$DRAIN timeout=$TIMEOUT nic=$([ $NET = 1 ] && echo $NIC || echo none) cmds=${#CMDS[@]} -> $LOG" >&2
feed | "$TO" "$TIMEOUT" "$QEMU" -kernel "$KERNEL" -smp "$SMP" -m 1024 -display none -serial stdio \
      "${MACHINE[@]}" ${NETARGS[@]+"${NETARGS[@]}"} >"$LOG" 2>&1
rc=$?
# 124 = gtimeout fired (expected: we always run to the drain window).
echo "qemu-test: qemu exit=$rc (124 = timed out, normal for a fixed-length session)" >&2
cat "$LOG"
