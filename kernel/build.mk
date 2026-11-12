# kernel/build.mk: the kernel image, its object list and the final link.
# Program blobs (kernel/*_blob.o) are built by user/apps/build.mk; the font blobs too.

# Generic kernel compile rules. Defined here, not toolchain.mk, so they come after
# user/lib's `user/%.o` rule: macOS GNU Make 3.81 resolves overlapping pattern rules
# by definition order (3.82+/Linux picks the shortest stem), so the user rule must be
# seen first or kernel CFLAGS would wrongly compile user code.
%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# KERNEL (owos.bin / owos-x86_64.bin) is set per-ARCH in toolchain.mk.
ISO    := owos.iso
ISODIR := isodir

# Objects are emitted in place (kernel/foo.o), shared by both arches. The stamp stores
# the active compiler configuration, so switching ARCH or OPT_LEVEL rebuilds objects.
.PHONY: FORCE
FORCE:

$(ARCH_STAMP): FORCE
	@if [ ! -f $@ ] || [ "$$(cat $@)" != "$(ARCH):O$(OPT_LEVEL)" ]; then \
		rm -f .arch.*; \
		printf '%s\n' '$(ARCH):O$(OPT_LEVEL)' > $@; \
		sleep 1; \
		touch $@; \
	fi

# Every source in kernel/, kernel/arch/x86/ (the other arch/ dirs have their own builds) and drivers/<area>/ is
# linked. fontstore.c is generated, so it is named here rather than left to the wildcard. x86 files that differ by
# word size come as foo_32 / foo_64 pairs; the other ARCH's half is dropped.
X86_SKIP := $(if $(filter x86_64,$(ARCH)),_32,_64)
KSRCS := $(filter-out kernel/fontstore.c kernel/arch/x86/%$(X86_SKIP).c kernel/arch/x86/%$(X86_SKIP).S, \
           $(wildcard kernel/*.c kernel/*.S kernel/arch/x86/*.c kernel/arch/x86/*.S drivers/*/*.c drivers/*/*.S))
OBJS  := $(addsuffix .o,$(basename $(KSRCS))) kernel/fontstore.o kernel/appsyms.o \
         $(APPS:%=kernel/%_blob.o) $(DESKTOP_APPS:%=kernel/%_desktop_blob.o) \
         $(foreach i,$(ICON_NAMES),kernel/icon_$(subst -,_,$(i))_blob.o)

$(KERNEL): $(OBJS) $(LINKER)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc

# Force a rebuild of every object when the target ARCH changes (see ARCH_STAMP).
$(OBJS): $(ARCH_STAMP)

-include $(OBJS:.o=.d)
