# Third-party code

Everything here is fetched at build time (CMake `FetchContent`), pinned to a version, and none of it is copied into the repository. Each one's own licence file comes with its source.

| Library | Version | Licence | Used for |
|---|---|---|---|
| [SDL](https://github.com/libsdl-org/SDL) | `release-3.4.20` | zlib | window, input, audio output (`host/`) |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `vulkan-sdk-1.4.363.0` | Apache-2.0 (and MIT) | Vulkan API headers |
| [volk](https://github.com/zeux/volk) | `vulkan-sdk-1.4.363.0` | MIT | Vulkan function loader |
| [Dear ImGui](https://github.com/ocornut/imgui) | `c1aaac65e` | MIT | the settings menu |
| [pl_mpeg](https://github.com/phoboslab/pl_mpeg) | `c871f2be0` | MIT | the intro movie's MPEG-1 video and stream separation (`msr/src/movie.cpp`) |
| [AMD FidelityFX FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | `a21ffb8f6` (FSR 1.0): `ffx-fsr/ffx_a.h`, `ffx-fsr/ffx_fsr1.h`, `license.txt` | MIT (AMD) | FSR 1 upscaling in the present pass (`host/src/fsr/`) |
| [SMAA](https://github.com/iryoku/smaa) | `71c806a83`: `SMAA.hlsl`, `Textures/AreaTex.h`, `Textures/SearchTex.h`, `LICENSE.txt` | MIT (Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez) | SMAA 1x anti-aliasing in the present pass (`host/src/smaa/`) |

## Data

| Data | Version | Licence | Used for |
|---|---|---|---|
| [GNU Unifont](https://unifoundry.com/unifont/), Japanese variant (`unifont_jp`) | `18.0.01` (`unifont_jp-18.0.01.hex.gz`, SHA-256 `c9c3af33…948736b`) | GPL-2.0-or-later with the GNU font embedding exception, and OFL-1.1 | the glyphs served in place of the boot ROM's font (`msr/src/romfont_glyphs.inc`) |

FSR 1's and SMAA's files are single files, not libraries: CMake downloads each from its pinned commit into `build/third_party` and checks its SHA-256 (`CMakeLists.txt`, `qilin_fetch_file`). Their shaders are compiled once to SPIR-V and the result is committed (`host/src/fsr/fsr_spirv.inc`, `host/src/smaa/smaa_spirv.inc`, by `tools/fsr_spirv.sh` and `tools/smaa_spirv.sh`, as Grabbed by the Ghoulies--QilinRecomp's are), so building needs no shader compiler; the SPIR-V is a derivative of those MIT sources.

Unlike the libraries, these glyphs are committed: `tools/make_romfont.py` generates `msr/src/romfont_glyphs.inc` from the pinned file (the developer downloads it; the script checks its hash), as `tools/build_shaders.py` does the shaders. The generated file is a derivative of Unifont and keeps its licence, stated at its top.

`dreamcast/` and `host/` come from Toy Commander--QilinRecomp (same owner, GPL-3.0, merged up to commit `0c4789e`); the present pass's SMAA and FSR from Grabbed by the Ghoulies--QilinRecomp (same owner, GPL-3.0).

Tools run as references and never linked or copied: GNU binutils (decoder checks), QEMU (the differential test), Flycast (planned, for comparison).

Where a format or piece of hardware is implemented from public documentation, the source is named in the code or in `docs/`. For example, the ADX sound decoder (`msr/src/adx.cpp`) follows the format's description in Wikipedia's "ADX (file format)".
