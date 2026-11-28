# The game's details for the packaging scripts (sourced): game=msr.
# Everything game-specific in a download comes from here: the README
# templates' @NAME@ places (fill) and kit/game.txt (write_game_txt), which is
# all the launcher knows about the game (tools/player-kit/launcher/ADOPTING.md).
# (Toy Commander's, for this project's one game.)
REPO="Qilin1066/MetropolisStreetRacer--QilinRecomp"
case "$game" in
msr)
    TITLE="Metropolis Street Racer"; PACKAGE="MetropolisStreetRacer--QilinRecomp"; EXE="MetropolisStreetRacer.exe"; BOOT="MSR.BIN"
    BUILD_TITLE="Metropolis Street Racer (USA)"; RELEASE="the North American release, MK-51012, version 1.010"; PRODUCT="MK-51012"; DISC_VERSION="1.010"
    STUDIO="Bizarre Creations / Sega, Dreamcast, 2000"
    # One compiler's peak on the largest lifted file, with room to spare:
    # lifted_002.cpp (250 functions) peaked at 807 MB in zig's clang at -O2
    # with the kit's flags, the sound driver's arm_lifted_00.cpp at 332 MB.
    COMPILE_MEMORY_MB=1000
    ;;
*)
    echo "unknown game '$game' (msr)" >&2
    exit 2
    ;;
esac
# Fills a template's @NAME@ places with the game's details.
fill() {
    sed -e "s|@GAME@|$game|g" -e "s|@TITLE@|$TITLE|g" -e "s|@PACKAGE@|$PACKAGE|g" -e "s|@EXE@|$EXE|g" \
        -e "s|@BOOT@|$BOOT|g" -e "s|@BUILD_TITLE@|$BUILD_TITLE|g" -e "s|@RELEASE@|$RELEASE|g" -e "s|@STUDIO@|$STUDIO|g" \
        -e "s|@REPO@|$REPO|g" "$1"
}
# The licences of what every download holds, by component, into <lic>
# (LICENSES/, or the app's Contents/Resources/LICENSES):
#   add_licences <lic> <build dir> <source root>
# The project's own (GPL-3.0); SDL3, Dear ImGui, Vulkan-Headers and volk (the
# launcher's and the game's); the game's: pl_mpeg (the intro movie), AMD
# FidelityFX FSR 1 and SMAA (the picture), GNU Unifont (the boot ROM's font,
# unifont/README.txt), and libchdr (the .chd reader) with what it bundles:
# LZMA, miniz, dr_flac (their licence texts are in their sources) and zstd's
# decompressor, taken under its BSD licence (ZSTD_LICENSE: that file from
# zstd's own repository). The bundled compiler's are each script's own.
add_licences() {
    lic=$1; build=$2; src=$3
    mkdir -p "$lic/$PACKAGE" "$lic/SDL3" "$lic/Dear ImGui" "$lic/Vulkan-Headers" "$lic/volk" "$lic/pl_mpeg" \
             "$lic/AMD FidelityFX FSR 1" "$lic/SMAA" "$lic/GNU Unifont" "$lic/libchdr"
    cp "$src/LICENSE" "$lic/$PACKAGE/LICENSE.txt"  # GPL-3.0
    cp "$build/_deps/sdl3-src/LICENSE.txt" "$lic/SDL3/"
    imgui=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_IMGUI:PATH=//p' "$build/CMakeCache.txt")
    [ -n "$imgui" ] || imgui="$build/_deps/imgui-src"
    cp "$imgui/LICENSE.txt" "$lic/Dear ImGui/"
    cp "$build/_deps/vulkanheaders-src/LICENSE.md" "$lic/Vulkan-Headers/" 2>/dev/null || cp "$build/_deps/vulkanheaders-src/LICENSE"* "$lic/Vulkan-Headers/"
    cp "$build/_deps/volk-src/LICENSE.md" "$lic/volk/"
    cp "$src/tools/player-kit/pl_mpeg-LICENSE.txt" "$lic/pl_mpeg/LICENSE.txt"
    cp "$build/third_party/fsr1/LICENSE.txt" "$lic/AMD FidelityFX FSR 1/LICENSE.txt"
    cp "$build/third_party/smaa/LICENSE.txt" "$lic/SMAA/LICENSE.txt"
    cp "$src/tools/player-kit/unifont/README.txt" "$src/tools/player-kit/unifont/GPL-2.0.txt" \
       "$src/tools/player-kit/unifont/OFL-1.1.txt" "$lic/GNU Unifont/"
    chdr=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_LIBCHDR:PATH=//p' "$build/CMakeCache.txt")
    [ -n "$chdr" ] || chdr="$build/_deps/libchdr-src"
    cp "$chdr/LICENSE.txt" "$lic/libchdr/LICENSE.txt"
    cp "$chdr"/deps/lzma-*/LICENSE "$lic/libchdr/LZMA-LICENSE.txt"
    sed -n '/This is free and unencumbered/,/unlicense.org/p' "$chdr"/deps/miniz-*/miniz.c > "$lic/libchdr/miniz-LICENSE.txt"
    sed -n '/^This software is available as a choice/,$p' "$chdr/include/dr_libs/dr_flac.h" > "$lic/libchdr/dr_flac-LICENSE.txt"
    for f in miniz dr_flac; do [ -s "$lic/libchdr/$f-LICENSE.txt" ] || { echo "no licence text found for $f" >&2; exit 1; }; done
    cp "${ZSTD_LICENSE:?set ZSTD_LICENSE to zstd v1.5.7 LICENSE file}" "$lic/libchdr/zstd-LICENSE.txt"
}
# Writes kit/game.txt, one name=value a line:
#   write_game_txt <file> <exe> <compiler> [<compiler_args>]
# with Windows line ends where GAME_TXT_CRLF=1. Every key is written, the
# launcher's defaults too, so the file says the whole story: the game's
# names, the disc it builds from (`build` is identify.cpp's supported disc,
# which `qilin identify --build` checks), how it is recompiled, what a
# compile needs, how the game starts, and where releases are published.
# The steps: the game's program and its sound driver taken from the disc
# image (only those two files, 1.3 MB: the game reads the rest from the
# image itself), then each lifted to C++ into work/gen. The game is started
# with the image (run_args).
write_game_txt() {
    {
        printf 'game=%s\ntitle=%s\nexe=%s\nboot=%s\n' "$game" "$TITLE" "$2" "$BOOT"
        printf 'build=%s\nproduct=%s\ndisc_version=%s\nrelease=%s\n' "$BUILD_TITLE" "$PRODUCT" "$DISC_VERSION" "$RELEASE"
        printf 'package=%s\nrepo=%s\nrelease_tag_prefix=v\nrelease_asset_prefix=%s-\n' "$PACKAGE" "$REPO" "$PACKAGE"
        printf 'input=file\ninput_extensions=cue;gdi;chd\ninput_noun=disc\n'
        printf "input_hint=Choose your disc's .cue file (with its .bin tracks beside it), its .gdi or its .chd.\n"
        printf 'identify={qilin} identify {input} --json --build {game:build}\n'
        printf 'step.1={qilin} extract {input} --file MSR.BIN --file SOUND/MANATEE.DRV --out {work}/disc\n'
        printf 'step.2={qilin} lift {work}/disc/MSR.BIN --out {gen}\n'
        printf 'step.3={qilin} armlift {work}/disc/SOUND/MANATEE.DRV --out {gen}\n'
        printf 'compile_memory_mb=%s\nbuild_space_mb=2048\ncache_mb=2048\nrun_args={input}\n' "$COMPILE_MEMORY_MB"
        printf 'compiler=%s\n' "$3"
        if [ -n "${4:-}" ]; then printf 'compiler_args=%s\n' "$4"; fi
    } | if [ "${GAME_TXT_CRLF:-0}" = 1 ]; then sed 's/$/\r/'; else cat; fi > "$1"
}
