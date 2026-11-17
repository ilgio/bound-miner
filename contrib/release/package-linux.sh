#!/usr/bin/env bash
# Milestone 13: builds BOUND's programs for Linux from this checkout and packs them the way a
# release ships them:
#
#   dist/bound-<version>-<arch>-linux-gnu.tar.gz    bound-<version>/bin: boundd, bound-cli,
#                                                   bound-explorer, bound-seeder; share/bound: the
#                                                   systemd units and bound-server (milestone 14)
#
# <arch> is the machine's: x86_64, or aarch64 (64-bit ARM: Oracle's and AWS's ARM servers, a
# Raspberry Pi with a 64-bit system).
#
# The libraries (libsodium, LevelDB) come from vcpkg and are linked in statically, as is the C++
# runtime, so the programs need nothing but glibc: no packages to install. glibc is the one library
# that can't be linked in, so the build machine's glibc is the oldest the programs run on; the
# release workflow builds on Ubuntu 22.04 (glibc 2.35).
#
#   VCPKG_ROOT=~/vcpkg contrib/release/package-linux.sh [out-dir]
#
# Optional: BUILD_DIR (default build/package-linux), MAX_GLIBC (fail if the programs need a newer
# glibc than this), SKIP_TESTS=1.
set -euo pipefail

out=${1:-dist}
build=${BUILD_DIR:-build/package-linux}
[[ -f CMakeLists.txt && -d contrib/release ]] || { echo "run this at the repository's root" >&2; exit 1; }
[[ -n "${VCPKG_ROOT:-}" ]] || { echo "set VCPKG_ROOT to a vcpkg checkout" >&2; exit 1; }

case "$(uname -m)" in
  x86_64) arch=x86_64 triplet=x64-linux ;;
  aarch64 | arm64) arch=aarch64 triplet=arm64-linux ;;
  *) echo "no release build for $(uname -m)" >&2; exit 1 ;;
esac

echo "== Configuring $build ($arch)"
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBOUND_GUI=OFF \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET="$triplet" \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
echo "== Building (Release)"
cmake --build "$build"
if [[ "${SKIP_TESTS:-}" != 1 ]]; then
  echo "== Testing the programs about to be packed"
  ctest --test-dir "$build" --output-on-failure
fi

version=$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")
name=bound-$version
mkdir -p "$out"
out=$(cd "$out" && pwd)
stage=$out/$name
rm -rf "$stage"
echo "== Installing into $stage"
cmake --install "$build" --prefix "$stage" --strip
cp contrib/release/README.txt "$stage/"
# cmake --install puts the license texts under share/doc; the top of the archive is where people look.
mv "$stage/share/doc/bound/LICENSE" "$stage/"
mv "$stage/share/doc/bound/licenses" "$stage/"
rmdir "$stage/share/doc/bound" "$stage/share/doc"

echo "== Checking what the programs need"
programs=(boundd bound-cli bound-explorer bound-seeder)
for program in "${programs[@]}"; do
  file="$stage/bin/$program"
  # Only glibc's own libraries may be loaded from the system.
  if extra=$(ldd "$file" | grep -vE 'linux-vdso|ld-linux|/libc\.so|/libm\.so|/libpthread\.so|/libdl\.so|/librt\.so'); then
    echo "$program needs libraries that aren't in glibc:" >&2
    echo "$extra" >&2
    exit 1
  fi
  needs=$( (objdump -T "$file" | grep -o 'GLIBC_[0-9.]*' || true) | sed 's/GLIBC_//' | sort -V | tail -1)
  if [[ -n "${MAX_GLIBC:-}" && "$(printf '%s\n%s\n' "$needs" "$MAX_GLIBC" | sort -V | tail -1)" != "$MAX_GLIBC" ]]; then
    echo "$program needs glibc $needs, newer than $MAX_GLIBC" >&2
    exit 1
  fi
  line=$("$file" --version | head -1)
  [[ "$line" == "$program $version"* ]] || { echo "$program --version said: $line" >&2; exit 1; }
  echo "   $line; needs glibc $needs"
done
"$stage/bin/boundd" --regtest demo > /dev/null
echo "   boundd demo: mined, paid, reorganised"

echo "== Packing"
archive=$name-$arch-linux-gnu.tar.gz
# Fixed owners, order and times, so the archive depends only on what's in it.
epoch=$(git log -1 --format=%ct 2>/dev/null || date +%s)
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" -C "$out" -cf - "$name" | gzip -9n > "$out/$archive"
(cd "$out" && sha256sum "$archive")
