# PFAviz++ build (w64devkit / MinGW-w64 GCC).
#   make            release build -> build/PFAviz++.exe
#   make debug=1    debug build
#   make native=1   -march=native

CXX      ?= g++
WINDRES  ?= windres

# -ffp-contract=off and no -ffast-math: every float expression copied from the original
# must round exactly as the original MSVC x64 SSE2 build did (DESIGN.md R6).
CXXFLAGS := -std=c++20 -municode -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
            -ffp-contract=off -fno-fast-math -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type \
            -Isrc -Ires -Ivendor/archive/include -MMD -MP
# Compressed and archived MIDI files (song/Archive.cpp): static libarchive with zlib, bzip2, xz, zstd
# and lz4 for MinGW-w64, headers and libraries in vendor/archive
ARCHIVE_LIBS := -Lvendor/archive/lib -larchive -lzstd -llzma -llz4 -lbz2 -lzs -lbcrypt

LDFLAGS  := -municode -mwindows -static
LDLIBS   := -ld3d9 -lwinmm -lcomctl32 -lcomdlg32 -lshell32 -lole32 -luuid -lgdi32 -luser32 -lavrt -lwindowscodecs $(ARCHIVE_LIBS)

ifeq ($(debug),1)
    CXXFLAGS += -O0 -g
    OUT := build/debug
else
    CXXFLAGS += -O2 -march=x86-64-v2
    LDFLAGS  += -s
    OUT := build
endif
ifeq ($(native),1)
    CXXFLAGS += -march=native
endif

SRCS := $(wildcard src/*.cpp src/*/*.cpp)
OBJS := $(patsubst src/%.cpp,$(OUT)/obj/%.o,$(SRCS)) $(OUT)/obj/app.res.o
TARGET := $(OUT)/PFAviz++.exe

.PHONY: all clean
all: $(TARGET)

# Local additions (not published), such as the test harness
-include local.mk

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(OUT)/obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/obj/app.res.o: res/app.rc res/resource.h res/app.manifest
	@mkdir -p $(dir $@)
	$(WINDRES) --codepage=65001 -Ires -O coff $< $@

clean:
	rm -rf build

-include $(OBJS:.o=.d)

$(OUT)/extended_keys.exe: tests/extended_keys.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) $(filter-out -mwindows,$(LDFLAGS)) -mconsole $^ -o $@ $(LDLIBS)
