# Third-party code

Everything here is fetched at build time (CMake `FetchContent`), pinned to a version, and none of it is copied into the repository. Each one's own licence file comes with its source.

| Library | Version | Licence | Used for |
|---|---|---|---|
| [SDL](https://github.com/libsdl-org/SDL) | `release-3.4.20` | zlib | window, input, audio output (`host/`) |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `vulkan-sdk-1.4.363.0` | Apache-2.0 (and MIT) | Vulkan API headers |
| [volk](https://github.com/zeux/volk) | `vulkan-sdk-1.4.363.0` | MIT | Vulkan function loader |
| [Dear ImGui](https://github.com/ocornut/imgui) | `c1aaac65e` | MIT | the settings menu |

`dreamcast/` and `host/` come from Toy Commander--QilinRecomp (same owner, GPL-3.0, commit `085a3fd`).

Tools run as references and never linked or copied: GNU binutils (decoder checks), QEMU (the differential test), Flycast (planned, for comparison).

Where a format or piece of hardware is implemented from public documentation, the source is named in the code or in `docs/`.
