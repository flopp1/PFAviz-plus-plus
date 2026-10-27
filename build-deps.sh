#!/bin/sh
# Builds vendor/archive: a static libarchive for MinGW-w64 with the compression libraries it reads
# MIDI files through (zlib, bzip2, xz, zstd, lz4), headers and licenses. Run it once from the
# w64devkit shell (it needs curl, which Windows 10 and later include, and w64devkit's cmake):
#
#   sh build-deps.sh
#
# The sources are pinned releases, checked against their SHA-256 before use and kept in
# build/deps/src (a tarball already there is not downloaded again). Only libarchive's readers are
# built: no tools, tests, crypto, XML or iconv. 7z and RAR (v4 and v5) are part of libarchive itself.
set -e
cd "$(dirname "$0")"
OUT=vendor/archive
SRC=build/deps/src
BLD=build/deps/build
PFX=$PWD/build/deps/prefix
mkdir -p "$SRC" "$BLD" "$PFX/include" "$PFX/lib"

# get <url> <sha256>: the tarball in $SRC, verified, and extracted there
get() {
    f="$SRC/${1##*/}"
    if [ ! -f "$f" ]; then
        echo "--- downloading ${1##*/}"
        curl -fL --retry 3 -o "$f.part" "$1"
        mv "$f.part" "$f"
    fi
    echo "$2  $f" | sha256sum -c - > /dev/null || { echo "ERROR: ${1##*/} does not match its SHA-256 (delete it and retry)"; exit 1; }
    # (zstd's tests hold symlinks busybox tar cannot make on Windows: not needed, so its errors are let
    # pass as long as the source folder is there)
    d="${f%.tar.*}"
    tar -xf "$f" -C "$SRC" 2> /dev/null || [ -f "$d/README.md" ] || [ -f "$d/README" ] || { echo "ERROR: ${1##*/} could not be extracted"; exit 1; }
}
get https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.xz d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3
get https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
get https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4.tar.gz 0014c7886930454fe8bd4228665b51af55eeae560ea135c9c4cd33f55b2591d9
get https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
get https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz 537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b
get https://github.com/libarchive/libarchive/releases/download/v3.8.9/libarchive-3.8.9.tar.xz 888c934f9d95648ecb9163dc8e23ab80a476ecb81a8f1154704a227b5b676dde

COMMON="-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PFX -DCMAKE_PREFIX_PATH=$PFX -DBUILD_SHARED_LIBS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5"
# cm <name> <source dir> [cmake options...]: configured, built and installed into $PFX
cm() {
    name="$1"; dir="$2"; shift 2
    echo "--- building $name"
    rm -rf "$BLD/$name"
    { cmake -G "MinGW Makefiles" -S "$dir" -B "$BLD/$name" $COMMON "$@" && cmake --build "$BLD/$name" -j 8 && cmake --install "$BLD/$name"; } \
        > "$BLD/$name.log" 2>&1 || { tail -30 "$BLD/$name.log"; exit 1; }
}

cm zlib "$SRC/zlib-1.3.2" -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_BUILD_TESTING=OFF

echo "--- building bzip2"   # no CMake upstream: seven C files
rm -rf "$BLD/bzip2"
mkdir -p "$BLD/bzip2"
for f in blocksort huffman crctable randtable compress decompress bzlib; do
    gcc -O2 -D_FILE_OFFSET_BITS=64 -c "$SRC/bzip2-1.0.8/$f.c" -o "$BLD/bzip2/$f.o"
done
rm -f "$PFX/lib/libbz2.a"
ar rcs "$PFX/lib/libbz2.a" "$BLD"/bzip2/*.o
cp "$SRC/bzip2-1.0.8/bzlib.h" "$PFX/include/"

cm xz "$SRC/xz-5.8.4" -DXZ_NLS=OFF -DXZ_DOC=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF -DXZ_TOOL_LZMADEC=OFF \
   -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF -DXZ_TOOL_SYMLINKS=OFF -DXZ_TOOL_SYMLINKS_LZMA=OFF -DBUILD_TESTING=OFF
cm zstd "$SRC/zstd-1.5.7/build/cmake" -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF \
   -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_CONTRIB=OFF
cm lz4 "$SRC/lz4-1.10.0/build/cmake" -DLZ4_BUILD_CLI=OFF -DLZ4_BUILD_LEGACY_LZ4C=OFF -DBUILD_STATIC_LIBS=ON
cm libarchive "$SRC/libarchive-3.8.9" \
   -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_LIBB2=OFF -DENABLE_LZO=OFF \
   -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF -DENABLE_WIN32_XMLLITE=OFF -DENABLE_PCREPOSIX=OFF \
   -DENABLE_PCRE2POSIX=OFF -DENABLE_ICONV=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_TAR=OFF \
   -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF -DENABLE_TEST=OFF -DENABLE_WERROR=OFF \
   -DENABLE_ZLIB=ON -DENABLE_BZip2=ON -DENABLE_LZMA=ON -DENABLE_ZSTD=ON -DENABLE_LZ4=ON \
   -DBZIP2_INCLUDE_DIR="$PFX/include" -DBZIP2_LIBRARIES="$PFX/lib/libbz2.a" \
   -DZLIB_INCLUDE_DIR="$PFX/include" -DZLIB_LIBRARY="$PFX/lib/libzs.a"
# Every codec must have been found: a missing one would silently drop a format
for h in HAVE_LIBZ HAVE_LIBBZ2 HAVE_LIBLZMA HAVE_LIBZSTD HAVE_LIBLZ4; do
    grep -q "#define $h 1" "$BLD/libarchive/config.h" || { echo "ERROR: libarchive was configured without $h"; exit 1; }
done

rm -rf "$OUT"
mkdir -p "$OUT/include" "$OUT/lib" "$OUT/licenses"
cp "$PFX/include/archive.h" "$PFX/include/archive_entry.h" "$OUT/include/"
for l in archive bz2 lz4 lzma zs zstd; do cp "$PFX/lib/lib$l.a" "$OUT/lib/"; done
cp "$SRC/libarchive-3.8.9/COPYING" "$OUT/licenses/libarchive.txt"
cp "$SRC/zlib-1.3.2/LICENSE" "$OUT/licenses/zlib.txt"
cp "$SRC/bzip2-1.0.8/LICENSE" "$OUT/licenses/bzip2.txt"
cat "$SRC/xz-5.8.4/COPYING" "$SRC/xz-5.8.4/COPYING.0BSD" > "$OUT/licenses/xz.txt"
cp "$SRC/zstd-1.5.7/LICENSE" "$OUT/licenses/zstd.txt"
cp "$SRC/lz4-1.10.0/LICENSE" "$OUT/licenses/lz4.txt"
echo "--- done: $OUT"
