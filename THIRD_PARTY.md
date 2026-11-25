# Third-party code

Everything here is fetched at build time (CMake `FetchContent`), pinned to a version, and none of it is copied into the repository. Each one's own licence file comes with its source.

| Library | Version | Licence | Used for |
|---|---|---|---|
| [SDL](https://github.com/libsdl-org/SDL) | `release-3.4.20` | zlib | window, input, audio output (`host/`) |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `vulkan-sdk-1.4.363.0` | Apache-2.0 (and MIT) | Vulkan API headers |
| [volk](https://github.com/zeux/volk) | `vulkan-sdk-1.4.363.0` | MIT | Vulkan function loader |
| [Dear ImGui](https://github.com/ocornut/imgui) | `c1aaac65e` | MIT | the settings menu |
| [pl_mpeg](https://github.com/phoboslab/pl_mpeg) | `c871f2be0` | MIT | the intro movie's MPEG-1 video and stream separation (`msr/src/movie.cpp`) |

## Data

| Data | Version | Licence | Used for |
|---|---|---|---|
| [GNU Unifont](https://unifoundry.com/unifont/), Japanese variant (`unifont_jp`) | `18.0.01` (`unifont_jp-18.0.01.hex.gz`, SHA-256 `c9c3af33…948736b`) | GPL-2.0-or-later with the GNU font embedding exception, and OFL-1.1 | the glyphs served in place of the boot ROM's font (`msr/src/romfont_glyphs.inc`) |

Unlike the libraries, these glyphs are committed: `tools/make_romfont.py` generates `msr/src/romfont_glyphs.inc` from the pinned file (the developer downloads it; the script checks its hash), as `tools/build_shaders.py` does the shaders. The generated file is a derivative of Unifont and keeps its licence, stated at its top.

`dreamcast/` and `host/` come from Toy Commander--QilinRecomp (same owner, GPL-3.0, commit `085a3fd`).

Tools run as references and never linked or copied: GNU binutils (decoder checks), QEMU (the differential test), Flycast (planned, for comparison).

Where a format or piece of hardware is implemented from public documentation, the source is named in the code or in `docs/`. For example, the ADX sound decoder (`msr/src/adx.cpp`) follows the format's description in Wikipedia's "ADX (file format)".
