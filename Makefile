# ==========================================================================
# LABYRINTH-O — Makefile
# Cross-compiles for Organelle (armv7l / ARM Cortex-A9)
# Build via Docker: see Dockerfile
# ==========================================================================

CC      := g++
TARGET  := labyrinth
SRC     := labyrinth.cpp

# Organelle target: armv7l Linux, glibc 2.24 (debian:stretch)
ARCH    := -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard

CFLAGS  := -std=c++14 \
            $(ARCH) \
            -O2 \
            -Wall \
            -fexceptions \
            -DNDEBUG

# Static libstdc++ so the binary is self-contained on the Organelle
LDFLAGS := -lasound \
            -lpthread \
            -lm \
            -static-libstdc++ \
            -static-libgcc

# Default: build the binary
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Build complete: $(TARGET)"
	@file $(TARGET)

# Quick syntax check (host compiler — no ARM cross-compile needed)
check:
	g++ -std=c++14 -fsyntax-only -Wall -I/usr/include/alsa $(SRC)

clean:
	rm -f $(TARGET) LabyrinthO.zip
	rm -rf LabyrinthO/

# Package for Organelle web upload (http://organelle.local)
# Creates LabyrinthO.zip — upload via the Organelle web interface
# The Organelle will extract it to /usbdrive/Patches/LabyrinthO/
# No main.pd → Organelle falls back to run.sh → launches the C++ binary
zip: $(TARGET)
	rm -rf LabyrinthO
	mkdir -p LabyrinthO
	cp $(TARGET) run.sh config.json LabyrinthO/
	chmod +x LabyrinthO/$(TARGET) LabyrinthO/run.sh
	zip -r LabyrinthO.zip LabyrinthO/
	rm -rf LabyrinthO/
	@echo "Created LabyrinthO.zip — upload at http://organelle.local"
	@ls -lh LabyrinthO.zip

# Deploy directly to a mounted Organelle USB drive (alternative to zip upload)
deploy:
	mkdir -p /Volumes/ORGANELLE/Patches/LabyrinthO
	cp $(TARGET) run.sh config.json /Volumes/ORGANELLE/Patches/LabyrinthO/
	chmod +x /Volumes/ORGANELLE/Patches/LabyrinthO/$(TARGET)
	sync
	@echo "Deployed to /Volumes/ORGANELLE/Patches/LabyrinthO/"

.PHONY: all check clean zip deploy
