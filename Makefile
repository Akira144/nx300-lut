# lutd - NX300 in-camera LUT for JPEGs
#
#   make                      build for this machine (desktop test)
#   make arm CROSS=... JPEG_ARM=...   build a static armel binary for the camera
#   make clean

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
LDLIBS   ?= -ljpeg

CROSS    ?= arm-linux-gnueabi-
JPEG_ARM ?= ../arm-jpeg

all: lutd

lutd: src/lutd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

arm: src/lutd.c
	$(CROSS)gcc $(CFLAGS) -march=armv7-a -static \
		-I$(JPEG_ARM)/include -o lutd.arm $< \
		-L$(JPEG_ARM)/lib -ljpeg
	@echo
	@file lutd.arm || true
	@ls -l lutd.arm

clean:
	rm -f lutd lutd.arm

.PHONY: all arm clean
