# Dockerfile — PullStretch cross-compilation environment
# Ubuntu 22.04 + arm-none-eabi toolchain + SWIG
# Same base as crossfilter/whirlpool/vectormix/tract/specgrn/rainchord/morphgrain

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-arm-none-eabi binutils-arm-none-eabi \
        libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib \
        swig make zip ca-certificates \
    && rm -rf /var/lib/apt/lists/*
RUN arm-none-eabi-g++ --version && swig -version

WORKDIR /build
