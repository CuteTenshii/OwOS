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

# Every source in boot/, kernel/ (arch/ has its own builds) and drivers/<area>/ is linked.
# fontstore.c is generated, so it is named here rather than left to the wildcard.
KSRCS := $(filter-out kernel/fontstore.c,$(wildcard boot/*.S kernel/*.c kernel/*.S drivers/*/*.c drivers/*/*.S))
OBJS  := $(addsuffix .o,$(basename $(KSRCS))) kernel/fontstore.o kernel/appsyms.o \
         $(APPS:%=kernel/%_blob.o) $(DESKTOP_APPS:%=kernel/%_desktop_blob.o) \
         $(foreach i,$(ICON_NAMES),kernel/icon_$(subst -,_,$(i))_blob.o)

$(KERNEL): $(OBJS) $(LINKER)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc

# Force a rebuild of every object when the target ARCH changes (see ARCH_STAMP).
$(OBJS): $(ARCH_STAMP)

# Blob objects have explicit rules in user/apps/build.mk. Keep their generated
# object format tied to the same architecture stamp as compiled sources.
$(filter %_blob.o,$(OBJS)): $(ARCH_STAMP)

-include $(OBJS:.o=.d)
