# toolchain.mk — compiler/linker flags and the generic kernel compile rules,
# shared by every other fragment.

# Version string baked into the build: the current git tag, else the short commit
# hash when untagged (git describe --always), with a -dirty suffix when the tree
# has uncommitted changes. Overridable on the command line (`make OWOS_VERSION=1.2`)
# or env; falls back to "dev" outside a git checkout. Passed to both the kernel and
# user builds via -DOWOS_VERSION.
OWOS_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
VERSION_DEF  := -DOWOS_VERSION='"$(OWOS_VERSION)"'

CC := gcc

# Freestanding: no host headers/libc, no PIE, no stack protector, no red zone.
# -mno-sse/-mno-mmx: the kernel never enables SSE (CR4.OSFXSR), so GCC must not
# auto-vectorize (e.g. a 16-byte init) into xmm/mmx instructions — those #UD.
# Device drivers live under drivers/<type>/; each subdir is added to the include
# path so the flat `#include "foo.h"` convention keeps resolving everywhere.
DRVDIRS := storage net audio usb input video acpi bus
CFLAGS  := -m32 -std=gnu11 -ffreestanding -fno-pie -fno-stack-protector \
           -mno-sse -mno-mmx -Wall -Wextra $(VERSION_DEF) -Ikernel -Ikernel/services $(addprefix -Idrivers/,$(DRVDIRS))
LDFLAGS := -m32 -ffreestanding -nostdlib -no-pie -T linker.ld

# Generic kernel compile rules. User code under user/ has a more-specific rule in
# user/lib/build.mk (UCFLAGS), which make prefers for those paths.
%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
