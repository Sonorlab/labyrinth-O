# ==========================================================================
# LABYRINTH-O — Dockerfile
# Cross-compilation environment: arm32v7/debian:stretch
# glibc 2.24 — matches Organelle Linux kernel 3.14.14+
#
# Build:
#   docker build --platform linux/arm/v7 -t labyrinth-builder .
#
# Compile:
#   docker run --platform linux/arm/v7 --rm -v $(pwd):/build labyrinth-builder make
# ==========================================================================

FROM arm32v7/debian:stretch

LABEL maintainer="Sonor Lab / Arthur Vincent"
LABEL description="LABYRINTH-O cross-compilation for Organelle (armv7l)"

# Debian Stretch is EOL — redirect to archive mirrors before updating
RUN sed -i 's|http://deb.debian.org/debian|http://archive.debian.org/debian|g' /etc/apt/sources.list \
 && sed -i 's|http://security.debian.org/debian-security|http://archive.debian.org/debian-security|g' /etc/apt/sources.list \
 && sed -i '/stretch-updates/d' /etc/apt/sources.list \
 && apt-get update -o Acquire::Check-Valid-Until=false \
 && apt-get install -y \
    build-essential \
    libasound2-dev \
    libpthread-stubs0-dev \
    file \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Default command: run make
CMD ["make"]
