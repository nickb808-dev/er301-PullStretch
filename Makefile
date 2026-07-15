# Makefile — PullStretch ER-301 package
# Follows the same toolchain as crossfilter/whirlpool/vectormix/tract/specgrn/rainchord/morphgrain/gaborscatter.
# CRITICAL: NO -include on OBJ_WRAP (compat_swig.h conflicts with <cstdio>).

ER301_SDK ?= $(HOME)/er-301
TOOLCHAIN ?= docker

PKG     := pullstretch
MODULE  := libpullstretch
VERSION := 0.7.0
ARCH    := am335x

SRCDIR   := src
OUTDIR   := lib/$(ARCH)
OUTLIB   := $(OUTDIR)/$(MODULE).so
OBJS_DIR := $(OUTDIR)/obj

SWIG_FILE := $(SRCDIR)/$(MODULE).swig
SWIG_WRAP := $(SRCDIR)/$(MODULE)_wrap.cpp

CXXFLAGS_COMMON := \
	-std=c++11 \
	-ffunction-sections \
	-fdata-sections \
	-ffast-math \
	-fno-builtin-sincosf \
	-fno-stack-protector \
	-fno-exceptions \
	-D__DYNAMIC_REENT__ \
	-mabi=aapcs \
	-DNDEBUG \
	-D_GLIBCXX_USE_CXX11_ABI=0 \
	-I$(ER301_SDK) \
	-I$(ER301_SDK)/libs/lua54 \
	-I$(SRCDIR) \
	-Wall \
	-Wno-unused-parameter

CXXFLAGS_ARM := \
	-mcpu=cortex-a8 \
	-mfpu=neon \
	-mfloat-abi=hard \
	-Dfar=

CXXFLAGS_DSP   := $(CXXFLAGS_COMMON) -O2 $(CXXFLAGS_ARM)
CXXFLAGS_WRAP  := $(CXXFLAGS_COMMON) -Os $(CXXFLAGS_ARM)
# CRITICAL: no -ffast-math here — prevents recursive sincosf in compat.cpp
CXXFLAGS_COMPAT := \
	-std=c++11 \
	-ffunction-sections \
	-fdata-sections \
	-fno-stack-protector \
	-fno-exceptions \
	-D__DYNAMIC_REENT__ \
	-mabi=aapcs \
	-DNDEBUG \
	-D_GLIBCXX_USE_CXX11_ABI=0 \
	-I$(ER301_SDK) \
	-I$(SRCDIR) \
	-O1 \
	$(CXXFLAGS_ARM)

LDFLAGS := -nostdlib -nodefaultlibs -r

CXX   := arm-none-eabi-g++
STRIP := arm-none-eabi-strip

DOCKER_IMAGE := er301-crosscompile:latest

OBJ_PS     := $(OBJS_DIR)/PullStretch.o
OBJ_WRAP   := $(OBJS_DIR)/libpullstretch_wrap.o
OBJ_COMPAT := $(OBJS_DIR)/compat.o

.PHONY: all swig build docker-image docker-build swig-docker pkg clean help
all: help

swig: $(SWIG_WRAP)

$(SWIG_WRAP): $(SWIG_FILE) $(SRCDIR)/PullStretch.h
	@echo ">>> SWIG: generating Lua wrapper..."
	swig -c++ -lua \
		-no-old-metatable-bindings \
		-nomoduleglobal \
		-small \
		-fvirtual \
		-fcompact \
		-I$(ER301_SDK) \
		-I$(SRCDIR) \
		-o $@ $<
	@echo ">>> SWIG done: $@"

build: $(OUTLIB)
	@echo ">>> Built: $(OUTLIB)"

$(OUTDIR):
	mkdir -p $@

$(OBJS_DIR): | $(OUTDIR)
	mkdir -p $@

$(OBJ_PS): $(SRCDIR)/PullStretch.cpp $(SRCDIR)/PullStretch.h | $(OBJS_DIR)
	@echo ">>> CC PullStretch.cpp"
	$(CXX) $(CXXFLAGS_DSP) -c -o $@ $<

# CRITICAL: NO -include compat_swig.h here — conflicts with <cstdio>
$(OBJ_WRAP): $(SWIG_WRAP) | $(OBJS_DIR)
	@echo ">>> CC libpullstretch_wrap.cpp"
	$(CXX) $(CXXFLAGS_WRAP) -c -o $@ $<

$(OBJ_COMPAT): $(SRCDIR)/compat.cpp | $(OBJS_DIR)
	@echo ">>> CC compat.cpp"
	$(CXX) $(CXXFLAGS_COMPAT) -c -o $@ $<

$(OUTLIB): $(OBJ_PS) $(OBJ_WRAP) $(OBJ_COMPAT) | $(OUTDIR)
	@echo ">>> LINK (relocatable) $(OUTLIB)"
	$(CXX) $(LDFLAGS) -o $@ $(OBJ_PS) $(OBJ_WRAP) $(OBJ_COMPAT)
	$(STRIP) --strip-unneeded $(OUTLIB)

docker-image:
	docker build -t $(DOCKER_IMAGE) -f Dockerfile .

swig-docker: docker-image
	$(eval SDK_ABS := $(shell realpath $(ER301_SDK) 2>/dev/null))
	@test -n "$(SDK_ABS)" || \
		{ echo "ERROR: ER301_SDK path '$(ER301_SDK)' does not exist."; exit 1; }
	@echo ">>> SWIG (inside Docker) ..."
	docker run --rm \
		-v "$(CURDIR)":/build \
		-v "$(SDK_ABS)":/er301_sdk \
		-w /build \
		$(DOCKER_IMAGE) \
		swig -c++ -lua \
			-no-old-metatable-bindings \
			-nomoduleglobal \
			-small \
			-fvirtual \
			-fcompact \
			-I/er301_sdk \
			-Isrc \
			-o $(SWIG_WRAP) $(SWIG_FILE)
	@echo ">>> SWIG done: $(SWIG_WRAP)"

docker-build: docker-image | $(OUTDIR)
	@test -f "$(SWIG_WRAP)" || \
		{ echo "ERROR: run 'make swig-docker ER301_SDK=~/er-301' first."; exit 1; }
	$(eval SDK_ABS := $(shell realpath $(ER301_SDK) 2>/dev/null))
	@test -n "$(SDK_ABS)" || \
		{ echo "ERROR: ER301_SDK path '$(ER301_SDK)' does not exist."; exit 1; }
	docker run --rm \
		-v "$(CURDIR)":/build \
		-v "$(SDK_ABS)":/er301_sdk \
		-w /build \
		$(DOCKER_IMAGE) \
		make build TOOLCHAIN=native ER301_SDK=/er301_sdk
	@echo ">>> Done: $(OUTLIB)"

PKGDIR  := build/$(ARCH)
PKGFILE := $(PKGDIR)/$(PKG)-$(VERSION).pkg

pkg: $(PKGFILE)

$(PKGFILE): $(OUTLIB) assets/toc.lua assets/PullStretch.lua | $(PKGDIR)
	@echo ">>> PKG $(PKGFILE)"
	cd assets && zip -j ../$(PKGFILE) toc.lua PullStretch.lua
	cd $(OUTDIR) && zip -j ../../$(PKGFILE) libpullstretch.so
	@echo ">>> Done: $(PKGFILE)"

$(PKGDIR):
	mkdir -p $@

clean:
	rm -f $(SWIG_WRAP) $(OUTLIB)
	rm -rf $(OBJS_DIR)

help:
	@echo ""
	@echo "PullStretch build targets (recommended order for macOS):"
	@echo "  make docker-image                           Build the Docker image (once)"
	@echo "  make swig-docker ER301_SDK=~/er-301         Generate SWIG wrapper"
	@echo "  make docker-build ER301_SDK=~/er-301        Cross-compile"
	@echo "  make pkg                                    Package → build/am335x/pullstretch-$(VERSION).pkg"
	@echo "  make clean                                  Remove generated files"
	@echo ""
