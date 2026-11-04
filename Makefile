# Top-level convenience targets for MintAMP.
#
# The Amiga cross-build remains in Makefile.amiga; this wrapper keeps the
# common release/clean commands short and applies release-only Workbench
# drawer geometry without modifying the source icon artwork.

.PHONY: all clean release

all:
	$(MAKE) -f Makefile.amiga all

clean:
	$(MAKE) -f Makefile.amiga clean
	rm -rf release

release:
	$(MAKE) -f Makefile.amiga release
	@# Classic Amiga drawer icons store the NewWindow height as a big-endian
	@# WORD at byte offset 84 (DiskObject header 78 + 6 bytes into DrawerData).
	@# The shared MintAMP drawer artwork is snapped at 265x64; keep its saved
	@# X/Y position and width, but make the packaged drawer 120 px high.
	@for icon in release/MintAMP-2.0-dev-680*.info; do \
		if [ -f "$$icon" ]; then \
			printf '\000\170' | dd of="$$icon" bs=1 seek=84 conv=notrunc 2>/dev/null; \
		fi; \
	done
