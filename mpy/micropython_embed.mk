# Regenerate MicroPython embed package (v1.25.0)
#
# Prerequisites:
#   git clone --depth 1 --branch v1.25.0 \
#       https://github.com/micropython/micropython.git /tmp/micropython-1.25.0
#
# Usage (Linux/WSL):
#   make -f micropython_embed.mk
#
# Usage (Windows CMD / PowerShell — set MICROPYTHON_TOP explicitly):
#   make -f micropython_embed.mk MICROPYTHON_TOP=C:/tmp/micropython-1.25.0

MICROPYTHON_TOP ?= /mnt/c/tmp/micropython-1.25.0
PACKAGE_DIR     := ../components/micropython_embed

# Must be absolute — embed.mk is invoked from a build subdir
QSTR_DEFS       := $(CURDIR)/qstrdefsport.h

.PHONY: all copy

all: copy
	$(MAKE) -f $(MICROPYTHON_TOP)/ports/embed/embed.mk \
		MICROPYTHON_TOP=$(MICROPYTHON_TOP) \
		PACKAGE_DIR=$(PACKAGE_DIR) \
		QSTR_DEFS=$(QSTR_DEFS)
	rm -rf $(CURDIR)/build-embed

copy:
	mkdir -p $(PACKAGE_DIR)
	cp -r CMakeLists.txt mpconfigport.h qstrdefsport.h $(PACKAGE_DIR)/

