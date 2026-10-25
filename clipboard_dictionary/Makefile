SWIFTC ?= swiftc

APP_NAME := Woerterbuch
APP := build/$(APP_NAME).app
CONTENTS := $(APP)/Contents
EXECUTABLE := $(CONTENTS)/MacOS/$(APP_NAME)
SOURCE := Sources/main.swift
PLIST := Resources/Info.plist
PKGINFO := Resources/PkgInfo

.PHONY: all clean run test

all: $(EXECUTABLE)

$(EXECUTABLE): $(SOURCE) $(PLIST) $(PKGINFO)
	@mkdir -p $(CONTENTS)/MacOS $(CONTENTS)/Resources
	cp $(PLIST) $(CONTENTS)/Info.plist
	cp $(PKGINFO) $(CONTENTS)/PkgInfo
	$(SWIFTC) -O -warnings-as-errors -target arm64-apple-macos13.0 \
		-module-cache-path build/module-cache \
		-framework AppKit $(SOURCE) -o $(EXECUTABLE)
	codesign --force --sign - $(APP)

run: $(EXECUTABLE)
	open $(APP)

test: $(EXECUTABLE)
	./test/test.sh

clean:
	rm -rf build
