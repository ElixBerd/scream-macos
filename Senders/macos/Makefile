# Scream for macOS: virtual output device, its control tool, and the
# process-tap sender. `make` builds everything, `sudo make install` installs
# the driver and screamctl.

ARCHS   ?= arm64 x86_64
CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
ARCHFLAGS = $(foreach a,$(ARCHS),-arch $(a))
PREFIX  ?= /usr/local
HALDIR  = /Library/Audio/Plug-Ins/HAL

BUILD   = build
DRIVER  = $(BUILD)/ScreamAudio.driver

all: driver screamctl tap-sender

driver: $(DRIVER)/Contents/MacOS/ScreamAudio

$(DRIVER)/Contents/MacOS/ScreamAudio: driver/ScreamAudio.c driver/Info.plist
	mkdir -p $(DRIVER)/Contents/MacOS
	cp driver/Info.plist $(DRIVER)/Contents/Info.plist
	$(CC) $(ARCHFLAGS) -mmacosx-version-min=12.0 $(CFLAGS) -bundle \
	  -o $@ driver/ScreamAudio.c -framework CoreAudio -framework CoreFoundation
	codesign --force --sign - $(DRIVER)

screamctl: $(BUILD)/screamctl

$(BUILD)/screamctl: screamctl/screamctl.c
	mkdir -p $(BUILD)
	$(CC) $(ARCHFLAGS) -mmacosx-version-min=12.0 $(CFLAGS) \
	  -o $@ screamctl/screamctl.c -framework CoreAudio -framework CoreFoundation
	codesign --force --sign - $@

tap-sender: $(BUILD)/scream-sender

$(BUILD)/scream-sender: tap-sender/scream-sender.m tap-sender/Info.plist
	mkdir -p $(BUILD)
	$(CC) $(ARCHFLAGS) -mmacosx-version-min=14.2 $(CFLAGS) -fobjc-arc \
	  -o $@ tap-sender/scream-sender.m -framework CoreAudio -framework Foundation \
	  -Wl,-sectcreate,__TEXT,__info_plist,tap-sender/Info.plist
	codesign --force --sign - --identifier com.github.scream.sender $@

install: driver screamctl
	rm -rf $(HALDIR)/ScreamAudio.driver
	cp -R $(DRIVER) $(HALDIR)/
	mkdir -p $(PREFIX)/bin
	cp $(BUILD)/screamctl $(PREFIX)/bin/
	killall coreaudiod || true

uninstall:
	rm -rf $(HALDIR)/ScreamAudio.driver
	rm -f $(PREFIX)/bin/screamctl
	killall coreaudiod || true

clean:
	rm -rf $(BUILD)

.PHONY: all driver screamctl tap-sender install uninstall clean
