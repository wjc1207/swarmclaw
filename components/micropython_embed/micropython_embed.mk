# Regenerate MicroPython embed package (v1.25.0)
#
# Prerequisites:
#   git clone --depth 1 --branch v1.25.0 https://github.com/micropython/micropython.git /tmp/micropython-1.25.0
#
# Usage:
#   make -f micropython_embed.mk
#
# The generated output replaces py/, genhdr/, shared/, extmod/, and port/
# subdirectories in this component.  CMakeLists.txt and this Makefile are
# preserved.

MICROPYTHON_TOP ?= /tmp/micropython-1.25.0
PACKAGE_DIR     := $(CURDIR)
QSTR_DEFS       := port/qstrdefsport.h

.PHONY: all
all:
	$(MAKE) -f $(MICROPYTHON_TOP)/ports/embed/embed.mk \
		MICROPYTHON_TOP=$(MICROPYTHON_TOP) \
		PACKAGE_DIR=$(PACKAGE_DIR) \
		QSTR_DEFS=$(QSTR_DEFS)
