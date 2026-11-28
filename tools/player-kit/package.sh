#!/bin/sh
# Builds the game's Windows download (as Toy Commander's):
#   tools/player-kit/package.sh msr <version> <out.zip>
#
# Cross-compiles on Linux with llvm-mingw, then lays out:
#   <Package>/Play <Title>.exe, README.txt, LICENSES/
#   <Package>/kit/  qilin.exe, lib/libmsr_kit.a, lib/icon.o, include/,
#                   build-flags.txt, game.txt, files.txt, VERSION,
#                   toolchain/ (llvm-mingw, trimmed)
# Nothing in it is made from the game: the player's PC takes MSR.BIN and the
# sound driver from their own disc image, lifts them and compiles that code
# with the bundled compiler (Play <Title>.exe, the launcher:
# tools/player-kit/launcher, which reads kit/game.txt).
#
# Environment:
#   LLVM_MINGW_CROSS    the Linux-hosted llvm-mingw release, unpacked
#   LLVM_MINGW_WINDOWS  the x86_64 Windows-hosted release of the same version, unpacked
#   ZSTD_LICENSE        zstd v1.5.7's LICENSE file (its BSD licence, for LICENSES/libchdr)
#   BUILD_DIR           where to build (default build-player-kit)
#   CMAKE_EXTRA         more cmake arguments (e.g. -DFETCHCONTENT_SOURCE_DIR_IMGUI=...)
set -eu
game=$1
version=$2
out=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/games.sh"
build=${BUILD_DIR:-$root/build-player-kit}
stage=$(mktemp -d)
pkg="$stage/$PACKAGE"

# shellcheck disable=SC2086
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$here/mingw-cross.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DQILIN_PLAYER_KIT=ON -DQILIN_WERROR=ON ${CMAKE_EXTRA:-}
cmake --build "$build" --target qilin qilin_launcher msr_kit msr_kit_link_check --parallel
cmake --install "$build" --component "$game" --prefix "$pkg"

"$here/trim-toolchain.sh" "$LLVM_MINGW_WINDOWS" "$pkg/kit/toolchain"
cp "$build/qilin_launcher.exe" "$pkg/Play $TITLE.exe"
# What the launcher needs to know about the game (games.sh), and the
# compiler: the bundled llvm-mingw.
GAME_TXT_CRLF=1 write_game_txt "$pkg/kit/game.txt" "$EXE" "toolchain/bin/x86_64-w64-mingw32-clang++.exe"
fill "$here/README.txt.in" | sed 's/$/\r/' > "$pkg/README.txt"
printf '%s\n' "$version" > "$pkg/kit/VERSION"

# Every licence of what's included, by component (games.sh), and llvm-mingw's.
lic="$pkg/LICENSES"
add_licences "$lic" "$build" "$root"
sed 's/$/\r/' "$root/LICENSE" > "$lic/$PACKAGE/LICENSE.txt"  # GPL-3.0, with Windows line ends
mkdir -p "$lic/llvm-mingw"
mv "$pkg/kit/toolchain/licenses/"* "$lic/llvm-mingw/"
rmdir "$pkg/kit/toolchain/licenses"

# Every file the release holds (itself too): the launcher removes anything
# else from kit\ and LICENSES\, which an older version unpacked over this
# one left behind (tools/player-kit/launcher/tidy.hpp).
(cd "$pkg" && { find . -type f | sed 's|^\./||'; echo kit/files.txt; } | LC_ALL=C sort -u | sed 's/$/\r/' > kit/files.txt)

rm -f "$out"
(cd "$stage" && zip -q -r -9 "$out" "$PACKAGE")
rm -rf "$stage"
echo "Packaged $out ($(du -h "$out" | cut -f1))"
