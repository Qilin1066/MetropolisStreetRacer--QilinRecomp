// The guest address space: see memory.hpp. Runtime code: no std::format.
#include "memory.hpp"

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <vector>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#endif

namespace qilin::rt {

namespace {
constexpr uint64_t kSpace = 1ull << 32;
}  // namespace

#if defined(_WIN32)

// Windows maps one section at several addresses inside a reservation through
// placeholders (Windows 10 1803 and later): reserve the whole space as one
// placeholder, split off each view's range, and replace it with a view.
// The placeholder calls are kernelbase.dll's, from Windows 10 1803: found at
// run time, so no toolchain's import library has to provide them (MSVC's is
// onecore, MinGW's differ between releases), and an older Windows gets a
// plain message instead of a loader error.
namespace {
struct PlaceholderApi {
    decltype(&::VirtualAlloc2) virtual_alloc2 = nullptr;
    decltype(&::MapViewOfFile3) map_view_of_file3 = nullptr;
    decltype(&::UnmapViewOfFile2) unmap_view_of_file2 = nullptr;
};
const PlaceholderApi& placeholder_api() {
    static const PlaceholderApi api = [] {
        PlaceholderApi a;
        if (HMODULE k = GetModuleHandleW(L"kernelbase.dll")) {
            a.virtual_alloc2 = reinterpret_cast<decltype(a.virtual_alloc2)>(reinterpret_cast<void*>(GetProcAddress(k, "VirtualAlloc2")));
            a.map_view_of_file3 = reinterpret_cast<decltype(a.map_view_of_file3)>(reinterpret_cast<void*>(GetProcAddress(k, "MapViewOfFile3")));
            a.unmap_view_of_file2 = reinterpret_cast<decltype(a.unmap_view_of_file2)>(reinterpret_cast<void*>(GetProcAddress(k, "UnmapViewOfFile2")));
        }
        return a;
    }();
    return api;
}
}  // namespace

struct GuestMemory::Native {
    HANDLE section = nullptr;
    std::vector<void*> views;         // mapped views, to unmap
    std::vector<void*> placeholders;  // what is left of the reservation, to free
};

GuestMemory::GuestMemory() {
    native_ = new Native;
    const PlaceholderApi& api = placeholder_api();
    if (!api.virtual_alloc2 || !api.map_view_of_file3 || !api.unmap_view_of_file2) {
        error_ = "this Windows has no placeholder mappings (VirtualAlloc2): it needs Windows 10 version 1803 or later";
        release();
        return;
    }
    native_->section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kRamSize, nullptr);
    if (!native_->section) {
        error_ = "CreateFileMapping failed for main RAM";
        release();
        return;
    }
    uint8_t* base = static_cast<uint8_t*>(
        api.virtual_alloc2(nullptr, nullptr, kSpace, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
    if (!base) {
        error_ = "cannot reserve 4 GB of address space for the guest";
        release();
        return;
    }
    // Split the placeholder at each view (in address order) and map the view.
    uint64_t done = 0;  // offset up to which the reservation has been split
    for (unsigned i = 0; i < kRamViews; ++i) {
        const uint64_t at = ram_view(i);
        if (at > done) native_->placeholders.push_back(base + done);  // the gap before this view
        if (at + kRamSize < kSpace && !VirtualFree(base + at, kRamSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            error_ = "cannot split the guest reservation";
            base_ = base;
            release();
            return;
        }
        void* view = api.map_view_of_file3(native_->section, GetCurrentProcess(), base + at, 0, kRamSize,
                                    MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (!view) {
            native_->placeholders.push_back(base + at);
            error_ = "cannot map a view of main RAM";
            base_ = base;
            release();
            return;
        }
        native_->views.push_back(view);
        done = at + kRamSize;
    }
    if (done < kSpace) native_->placeholders.push_back(base + done);
    base_ = base;
    ram_view_ = base + ram_view(4);  // P1: 0x8C000000
}

void GuestMemory::release() {
    if (native_) {
        for (void* v : native_->views) placeholder_api().unmap_view_of_file2(GetCurrentProcess(), v, 0);
        for (void* p : native_->placeholders) VirtualFree(p, 0, MEM_RELEASE);
        if (native_->views.empty() && native_->placeholders.empty() && base_) VirtualFree(base_, 0, MEM_RELEASE);
        if (native_->section) CloseHandle(native_->section);
        delete native_;
        native_ = nullptr;
    }
    base_ = ram_view_ = nullptr;
}

#else

// POSIX: one shared memory object, mapped with MAP_FIXED at each view inside
// a PROT_NONE reservation.
struct GuestMemory::Native {
    int fd = -1;
};

GuestMemory::GuestMemory() {
    native_ = new Native;
#if defined(__linux__)
    native_->fd = memfd_create("qilin-ram", MFD_CLOEXEC);
#else
    char name[64];
    std::snprintf(name, sizeof name, "/qilin-ram-%ld", static_cast<long>(getpid()));
    native_->fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (native_->fd >= 0) shm_unlink(name);  // only this process's mappings keep it
#endif
    if (native_->fd < 0 || ftruncate(native_->fd, kRamSize) != 0) {
        error_ = "cannot create the shared memory object for main RAM";
        release();
        return;
    }
    void* base = mmap(nullptr, kSpace, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) {
        error_ = "cannot reserve 4 GB of address space for the guest";
        release();
        return;
    }
    base_ = static_cast<uint8_t*>(base);
    for (unsigned i = 0; i < kRamViews; ++i) {
        if (mmap(base_ + ram_view(i), kRamSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, native_->fd, 0) ==
            MAP_FAILED) {
            error_ = "cannot map a view of main RAM";
            release();
            return;
        }
    }
    ram_view_ = base_ + ram_view(4);  // P1: 0x8C000000
}

void GuestMemory::release() {
    if (base_) munmap(base_, kSpace);
    if (native_) {
        if (native_->fd >= 0) close(native_->fd);
        delete native_;
        native_ = nullptr;
    }
    base_ = ram_view_ = nullptr;
}

#endif

GuestMemory::~GuestMemory() { release(); }

}  // namespace qilin::rt
