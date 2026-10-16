# Scream for macOS.
#
#   make           build the driver, screamctl, the menu bar app and tap sender
#   make pkg       build build/Scream-<version>.pkg, the installer for users
#   sudo make install / uninstall   install from source without the pkg

VERSION ?= 0.2.0
ARCHS   ?= arm64 x86_64
CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
ARCHFLAGS = $(foreach a,$(ARCHS),-arch $(a))
PREFIX  ?= /usr/local
HALDIR  = /Library/Audio/Plug-Ins/HAL

BUILD   = build
DRIVER  = $(BUILD)/ScreamAudio.driver
APP     = $(BUILD)/Scream.app
PKG     = $(BUILD)/Scream-$(VERSION).pkg
COMMON  = common/scream_device.c common/scream_device.h

all: driver screamctl app tap-sender

# ---------------------------------------------------------------- driver

driver: $(DRIVER)/Contents/MacOS/ScreamAudio

$(DRIVER)/Contents/MacOS/ScreamAudio: driver/ScreamAudio.c driver/Info.plist
	rm -rf $(DRIVER)
	mkdir -p $(DRIVER)/Contents/MacOS
	cp driver/Info.plist $(DRIVER)/Contents/Info.plist
	plutil -replace CFBundleShortVersionString -string $(VERSION) $(DRIVER)/Contents/Info.plist
	$(CC) $(ARCHFLAGS) -mmacosx-version-min=12.0 $(CFLAGS) -bundle \
	  -o $@ driver/ScreamAudio.c -framework CoreAudio -framework CoreFoundation
	codesign --force --sign - $(DRIVER)

# ---------------------------------------------------------------- screamctl

screamctl: $(BUILD)/screamctl

$(BUILD)/screamctl: screamctl/screamctl.c $(COMMON)
	mkdir -p $(BUILD)
	$(CC) $(ARCHFLAGS) -mmacosx-version-min=12.0 $(CFLAGS) \
	  -o $@ screamctl/screamctl.c common/scream_device.c -framework CoreAudio -framework CoreFoundation
	codesign --force --sign - $@

# ---------------------------------------------------------------- menu bar app

app: $(APP)/Contents/MacOS/Scream

$(BUILD)/AppIcon.icns: app/AppIcon.png
	rm -rf $(BUILD)/AppIcon.iconset
	mkdir -p $(BUILD)/AppIcon.iconset
	for s in 16 32 128 256 512; do \
	  sips -z $$s $$s app/AppIcon.png --out $(BUILD)/AppIcon.iconset/icon_$${s}x$${s}.png >/dev/null; \
	  sips -z $$((s*2)) $$((s*2)) app/AppIcon.png --out $(BUILD)/AppIcon.iconset/icon_$${s}x$${s}@2x.png >/dev/null; \
	done
	iconutil -c icns -o $@ $(BUILD)/AppIcon.iconset

$(APP)/Contents/MacOS/Scream: app/ScreamMenu.m app/Info.plist $(COMMON) $(BUILD)/AppIcon.icns pkg/uninstall.sh
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources
	sed 's/VERSION/$(VERSION)/g' app/Info.plist > $(APP)/Contents/Info.plist
	cp $(BUILD)/AppIcon.icns pkg/uninstall.sh $(APP)/Contents/Resources/
	$(CC) $(ARCHFLAGS) -mmacosx-version-min=13.0 $(CFLAGS) -fobjc-arc \
	  -o $@ app/ScreamMenu.m common/scream_device.c \
	  -framework Cocoa -framework CoreAudio -framework ServiceManagement
	codesign --force --sign - $(APP)

# ---------------------------------------------------------------- tap sender

tap-sender: $(BUILD)/scream-sender

$(BUILD)/scream-sender: tap-sender/scream-sender.m tap-sender/Info.plist
	mkdir -p $(BUILD)
	$(CC) $(ARCHFLAGS) -mmacosx-version-min=14.2 $(CFLAGS) -fobjc-arc \
	  -o $@ tap-sender/scream-sender.m -framework CoreAudio -framework Foundation \
	  -Wl,-sectcreate,__TEXT,__info_plist,tap-sender/Info.plist
	codesign --force --sign - --identifier com.github.scream.sender $@

# ---------------------------------------------------------------- installer

pkg: $(PKG)

$(PKG): driver screamctl app pkg/distribution.xml pkg/resources/welcome.html pkg/scripts/preinstall pkg/scripts/postinstall
	rm -rf $(BUILD)/pkgroot
	mkdir -p $(BUILD)/pkgroot/Applications $(BUILD)/pkgroot$(HALDIR) $(BUILD)/pkgroot/usr/local/bin
	cp -R $(APP) $(BUILD)/pkgroot/Applications/
	cp -R $(DRIVER) $(BUILD)/pkgroot$(HALDIR)/
	cp $(BUILD)/screamctl $(BUILD)/pkgroot/usr/local/bin/
	pkgbuild --analyze --root $(BUILD)/pkgroot $(BUILD)/component.plist
	for i in 0 1 2 3 4 5; do plutil -replace $$i.BundleIsRelocatable -bool NO $(BUILD)/component.plist 2>/dev/null || break; done
	pkgbuild --root $(BUILD)/pkgroot --component-plist $(BUILD)/component.plist \
	  --scripts pkg/scripts --identifier com.github.elixberd.scream --version $(VERSION) \
	  --install-location / $(BUILD)/Scream-component.pkg
	sed 's/VERSION/$(VERSION)/g' pkg/distribution.xml > $(BUILD)/distribution.xml
	productbuild --distribution $(BUILD)/distribution.xml --resources pkg/resources \
	  --package-path $(BUILD) $@
	rm $(BUILD)/Scream-component.pkg

# ---------------------------------------------------------------- from source

install: driver screamctl app
	rm -rf $(HALDIR)/ScreamAudio.driver /Applications/Scream.app
	cp -R $(DRIVER) $(HALDIR)/
	cp -R $(APP) /Applications/
	mkdir -p $(PREFIX)/bin
	cp $(BUILD)/screamctl $(PREFIX)/bin/
	killall coreaudiod || true

uninstall:
	pkg/uninstall.sh

clean:
	rm -rf $(BUILD)

.PHONY: all driver screamctl app tap-sender pkg install uninstall clean
