// The port's screen (video.hpp).
#include "video.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "clock.hpp"
#include "game.hpp"
#include "guest.hpp"
#include "menu.hpp"
#include "pvr.hpp"
#include "qilin/host/display.hpp"
#include "qilin/host/paths.hpp"
#include "renderer.hpp"
#include "save_history.hpp"
#include "sound.hpp"
#include "settings.hpp"
#include "system.hpp"
#include "ta.hpp"

namespace dreamcast::video {

namespace rt = qilin::rt;

namespace {

std::unique_ptr<qilin::host::Display> g_display;
std::unique_ptr<renderer::Renderer> g_renderer;
Settings g_settings;
std::filesystem::path g_data;
std::unique_ptr<Menu> g_menu;
std::thread::id g_thread;
std::string g_shot_folder;
uint64_t g_shot_every = 60, g_pictures = 0;
uint64_t g_shot_from = 0, g_shot_to = UINT64_MAX;  // QILIN_SHOT's window of pictures
uint32_t g_scale = 2, g_last_drawn = 0;
bool g_drawn = false;  // a render since the last picture shown
// The widening the screen asks for (screen_widening), kept by the window's
// thread for the guest's.
std::atomic<float> g_screen_widening{1.0f};

void measure_screen() {
    const float aspect = g_display->screen_aspect();
    if (aspect > 0.0f) g_screen_widening.store(std::clamp(aspect * 0.75f, 1.0f, 3.0f), std::memory_order_relaxed);
}

// The registers the renderer reads (the console's names).
constexpr uint32_t kFbRCtrl = 0x005F8044u, kFbRSof1 = 0x005F8050u, kFbRSize = 0x005F805Cu, kFbWSof1 = 0x005F8060u,
                   kSpgControl = 0x005F80D0u;
constexpr uint32_t kParamBase = 0x005F8020u, kFbXClip = 0x005F8068u, kFbYClip = 0x005F806Cu, kIspBackgndD = 0x005F8088u,
                   kIspBackgndT = 0x005F808Cu, kTextControl = 0x005F80E4u, kIspFeedCfg = 0x005F8098u, kShadowScale = 0x005F8074u, kPalRamCtrl = 0x005F8108u, kPtAlphaRef = 0x005F811Cu, kFogTable = 0x005F8200u,
                   kFogColRam = 0x005F80B0u, kFogColVert = 0x005F80B4u, kFogDensity = 0x005F80B8u, kFogClampMax = 0x005F80BCu,
                   kFogClampMin = 0x005F80C0u;

float as_float(uint32_t w) {
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

// The background plane: ISP_BACKGND_T points at its instruction words and
// three vertices in the parameter memory (video memory's 32-bit view), which
// the game's library writes there itself. Drawn as the parallelogram they
// span, at ISP_BACKGND_D's depth.
void background(renderer::Source& src) {
    const uint32_t t = pvr_register(kIspBackgndT);
    const uint32_t at = (pvr_register(kParamBase) & 0x00F00000u) + ((t >> 3) & 0x1FFFFFu) * 4u;
    const uint32_t skip = (t >> 24) & 7u, vertex_words = 3u + skip, offset = t & 7u;
    if (t & (1u << 27)) throw rt::Stop{rt::Stop::Unsupported, 0, t};  // a background with two volumes
    if (at + (3u + (offset + 3u) * vertex_words) * 4u > (8u << 20)) throw rt::Stop{rt::Stop::Unsupported, 0, t};
    const auto word = [&](uint32_t k) {
        uint32_t w;
        std::memcpy(&w, vram() + at + k * 4u, 4);
        return w;
    };
    ta::Strip& s = src.background;
    s = {};
    s.list = ta::kOpaque;
    s.isp = word(0), s.tsp[0] = word(1), s.tcw[0] = word(2);
    s.textured = (s.isp >> 25) & 1u;
    const bool with_offset = (s.isp >> 24) & 1u, uv16 = (s.isp >> 22) & 1u;
    // The strip's PCW bits the renderer reads: texture, offset, Gouraud.
    s.pcw = (s.textured ? 0x08u : 0u) | (with_offset ? 0x04u : 0u) | ((s.isp >> 23) & 1u ? 0x02u : 0u);
    float depth = as_float(pvr_register(kIspBackgndD) & ~0xFu);
    ta::Vertex* v = src.background_vertices;
    for (uint32_t n = 0; n < 3; ++n) {
        uint32_t k = 3u + (offset + n) * vertex_words;
        ta::Vertex& o = v[n];
        o = {};
        o.x = as_float(word(k)), o.y = as_float(word(k + 1)), o.z = depth > 0.0f ? depth : as_float(word(k + 2));
        k += 3;
        if (s.textured) {
            if (uv16) {
                o.u[0] = as_float(word(k) & 0xFFFF0000u), o.v[0] = as_float(word(k) << 16);
                k += 1;
            } else {
                o.u[0] = as_float(word(k)), o.v[0] = as_float(word(k + 1));
                k += 2;
            }
        }
        const auto packed = [](uint32_t argb, float out[4]) {
            out[0] = float((argb >> 16) & 255u) / 255.0f, out[1] = float((argb >> 8) & 255u) / 255.0f;
            out[2] = float(argb & 255u) / 255.0f, out[3] = float(argb >> 24) / 255.0f;
        };
        packed(word(k), o.base[0]);
        if (with_offset) packed(word(k + 1), o.offset[0]);
        if (!std::isfinite(o.x) || !std::isfinite(o.y)) return;  // nothing set yet: no plane
    }
    // A strip A, B, C, D with D completing the parallelogram.
    v[3] = v[2];
    v[3].x = v[1].x + v[2].x - v[0].x, v[3].y = v[1].y + v[2].y - v[0].y;
    v[3].u[0] = v[1].u[0] + v[2].u[0] - v[0].u[0], v[3].v[0] = v[1].v[0] + v[2].v[0] - v[0].v[0];
    s.first = 0, s.count = 4;
}

void shot(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    const std::string path = g_shot_folder + "/frame_" + std::to_string(g_pictures) + ".ppm";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) rt::fatal("can't write %s", path.c_str());
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> rgb(rgba.size() / 4 * 3);
    for (size_t k = 0, o = 0; k + 3 < rgba.size(); k += 4, o += 3) rgb[o] = rgba[k], rgb[o + 1] = rgba[k + 1], rgb[o + 2] = rgba[k + 2];
    const bool written = std::fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
    if (std::fclose(f) != 0 || !written) rt::fatal("can't write %s", path.c_str());
}

// The texture packs' folders, as the settings say: in the data folder.
void set_texture_packs() {
    const std::filesystem::path folder = g_data / "textures";
    g_renderer->set_texture_packs(g_settings.texture_packs ? folder / "load" : std::filesystem::path{},
                                  g_settings.dump_textures ? folder / "dump" : std::filesystem::path{});
}

}  // namespace

void start() {
    g_data = qilin::host::user_folder();
    // A restore asked for from the menu: before anything reads the saves.
    if (std::string error; !apply_save_restore(g_data, error))
        rt::log("saves: the restore asked for wasn't done (%s); restore-save.txt is kept", error.c_str());
    g_settings = load_settings(g_data);
    game::set_console(g_settings.language, g_settings.stereo);
    sound::set_device(g_settings.sound_device);
    // The game's own sound option writes the console's setting: keep it.
    game::on_sound_mode([](bool stereo) {
        g_settings.stereo = stereo;
        if (!save_settings(g_data, g_settings)) rt::log("settings: couldn't save the sound mode");
    });
    qilin::host::Display::Options o;
    o.width = 1280, o.height = 960;
    const char* headless = std::getenv("QILIN_HEADLESS");
    o.headless = headless && *headless && *headless != '0';
    // QILIN_SCREEN=<width>x<height>: the headless target's size (the screen's
    // shape, which the picture is widened for), 1280x960 if not given.
    if (const char* screen = std::getenv("QILIN_SCREEN"); screen && o.headless) {
        unsigned w = 0, h = 0;
        if (std::sscanf(screen, "%ux%u", &w, &h) != 2 || w < 64 || h < 64 || w > 8192 || h > 8192)
            rt::fatal("QILIN_SCREEN must be <width>x<height>, 64 to 8192 each");
        o.width = w, o.height = h;
    }
    o.fullscreen = g_settings.fullscreen;
    o.monitor = g_settings.monitor;
    o.picture = picture_of(g_settings);
    qilin::host::Gpu::prefer(g_settings.gpu);
    g_display = std::make_unique<qilin::host::Display>(o);
    g_thread = std::this_thread::get_id();
    g_scale = g_settings.scale;
    if (g_scale == 0) {  // auto: the smallest whole multiple whose 480 picture lines fill the screen's height
        int h = 0;
        if (auto* w = static_cast<SDL_Window*>(g_display->sdl_window()))
            if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)))
                h = int(float(m->h) * (m->pixel_density > 0 ? m->pixel_density : 1.0f));
        g_scale = h > 0 ? static_cast<uint32_t>(std::clamp((h + 479) / 480, 1, 8)) : 2u;
    }
    if (const char* s = std::getenv("QILIN_SCALE")) g_scale = static_cast<uint32_t>(std::atoi(s));
    if (g_scale < 1 || g_scale > 8) rt::fatal("QILIN_SCALE must be 1 to 8");
    g_renderer = std::make_unique<renderer::Renderer>(g_display->gpu(), g_scale);
    g_renderer->set_anisotropy(g_settings.anisotropy);
    g_renderer->set_msaa(g_settings.msaa);
    set_texture_packs();
    if (!o.headless) {
        g_menu = std::make_unique<Menu>(*g_display, g_settings, g_data);
        g_menu->offer_frame_rates(game::frame_rate_choice());
        g_menu->set_running_scale(g_scale);
    }
    // QILIN_SHOT=<folder>[,<every>[,<from>-<to>]]: every <every>th picture (60), only from picture
    // <from> to <to> if given (a dense look at a moment without a shot of the whole run).
    if (const char* s = std::getenv("QILIN_SHOT")) {
        g_shot_folder = s;
        if (const size_t comma = g_shot_folder.find(','); comma != std::string::npos) {
            char* end = nullptr;
            g_shot_every = std::strtoull(g_shot_folder.c_str() + comma + 1, &end, 10);
            if (*end == ',') {
                g_shot_from = std::strtoull(end + 1, &end, 10);
                if (*end != '-') rt::fatal("QILIN_SHOT: <folder>,<every>,<from>-<to>");
                g_shot_to = std::strtoull(end + 1, &end, 10);
            }
            if (*end != '\0' || g_shot_every == 0 || g_shot_from > g_shot_to) rt::fatal("QILIN_SHOT: <folder>[,<every>[,<from>-<to>]], every at least 1");
            g_shot_folder.resize(comma);
        }
    }
    measure_screen();
    std::fprintf(stderr, QILIN_APP ": drawing with %s\n", g_display->device_name().c_str());
}

// QILIN_DUMP=<folder>,<picture>[,...]: main RAM (16 MB from 0x8C000000) to
// <folder>/ram_<picture>.bin when that picture is shown, for comparing a
// running game's state between pictures.
static void dump_ram_if_asked() {
    static std::string folder;
    static std::vector<uint64_t> at = [] {
        std::vector<uint64_t> v;
        const char* s = std::getenv("QILIN_DUMP");
        if (!s) return v;
        std::string all = s;
        const size_t comma = all.find(',');
        if (comma == std::string::npos) rt::fatal("QILIN_DUMP: <folder>,<picture>[,...]");
        folder = all.substr(0, comma);
        for (const char* c = all.c_str() + comma; *c == ',';) {
            char* end = nullptr;
            v.push_back(std::strtoull(c + 1, &end, 10));
            c = end;
        }
        return v;
    }();
    if (std::find(at.begin(), at.end(), g_pictures) == at.end()) return;
    const uint8_t* ram = rt::ram_range(0x8C000000u, 0x01000000u);
    if (!ram) rt::fatal("QILIN_DUMP: main RAM isn't mapped");
    const std::string path = folder + "/ram_" + std::to_string(g_pictures) + ".bin";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    const bool written = f && std::fwrite(ram, 1, 0x01000000u, f) == 0x01000000u;
    if (!f || std::fclose(f) != 0 || !written) rt::fatal("QILIN_DUMP: cannot write %s", path.c_str());
}
float screen_widening() { return g_screen_widening.load(std::memory_order_relaxed); }

void render() {
    if (!g_renderer) return;
    renderer::Source src;
    src.scene = &ta::scene();
    src.frame_buffer = pvr_register(kFbWSof1) & 0x00FFFFFCu;
    // The rectangle written: the frame buffer clip (inclusive).
    const uint32_t xc = pvr_register(kFbXClip), yc = pvr_register(kFbYClip);
    src.clip_x0 = xc & 0x7FFu, src.clip_x1 = (xc >> 16) & 0x7FFu;
    src.clip_y0 = yc & 0x3FFu, src.clip_y1 = (yc >> 16) & 0x3FFu;
    // The frame buffer's size: what the display reads (FB_R_SIZE, in 32-bit
    // words a line at FB_R_CTRL's depth, and lines), at least the clip.
    const uint32_t size = pvr_register(kFbRSize), depth = (pvr_register(kFbRCtrl) >> 2) & 3u;
    const uint32_t bytes_per_pixel = depth < 2 ? 2u : depth == 2 ? 3u : 4u;
    const uint32_t interlaced = (pvr_register(kSpgControl) >> 4) & 1u;
    src.width = std::max(((size & 0x3FFu) + 1u) * 4u / bytes_per_pixel, src.clip_x1 + 1u);
    src.height = std::max((((size >> 10) & 0x3FFu) + 1u) << interlaced, src.clip_y1 + 1u);
    if (src.width > 2048 || src.height > 1024) throw rt::Stop{rt::Stop::Unsupported, 0, size};
    src.palette_format = pvr_register(kPalRamCtrl) & 3u;
    src.stride = pvr_register(kTextControl) & 31u;
    src.punch_alpha_ref = pvr_register(kPtAlphaRef);
    src.presort = pvr_register(kIspFeedCfg) & 1u;
    src.shadow_scale = pvr_register(kShadowScale);
    src.fog_colour = pvr_register(kFogColRam) & 0x00FFFFFFu, src.fog_density = pvr_register(kFogDensity) & 0xFFFFu;
    src.fog_vertex_colour = pvr_register(kFogColVert) & 0x00FFFFFFu;
    src.clamp_max = pvr_register(kFogClampMax), src.clamp_min = pvr_register(kFogClampMin);
    for (uint32_t k = 0; k < 128; ++k) src.fog_table[k] = pvr_register(kFogTable + 4 * k) & 0xFFFFu;
    src.palette = pvr_palette();
    src.read = vram64_read;
    src.generation = vram64_generation;
    background(src);
    // The game's camera widened for the screen (game.hpp): the picture too,
    // but never a movie's, a 4:3 picture as it is.
    game::widen_to(screen_widening());
    src.widen = game::movie_playing() ? 1.0f : std::clamp(game::widening(), 1.0f, 3.0f);
    // QILIN_TRACE_RENDERS: each render's frame buffer and format, the frame
    // buffer the display reads, and any texture read from where a render wrote.
    static const bool trace = std::getenv("QILIN_TRACE_RENDERS") != nullptr;
    if (trace) {
        static uint64_t renders = 0;
        std::fprintf(stderr,
                     "render %llu (picture %llu): FB_W_SOF1 %06X FB_W_CTRL %08X FB_W_LINESTRIDE %u, %ux%u, clip x %08X y %08X, SCALER_CTL %08X, "
                     "REGION_BASE %06X, GLOB_TILE_CLIP %08X; shown FB_R_SOF1 %06X SOF2 %06X FB_R_CTRL %08X FB_R_SIZE %08X VO_STARTY %08X\n",
                     static_cast<unsigned long long>(renders++), static_cast<unsigned long long>(g_pictures), src.frame_buffer,
                     pvr_register(0x005F8048u), pvr_register(0x005F804Cu), src.width, src.height, pvr_register(kFbXClip), pvr_register(kFbYClip),
                     pvr_register(0x005F80F4u), pvr_register(0x005F802Cu), pvr_register(0x005F813Cu), pvr_register(kFbRSof1),
                     pvr_register(0x005F8054u), pvr_register(kFbRCtrl), pvr_register(kFbRSize), pvr_register(0x005F80F0u));
        for (const ta::Strip& s : src.scene->strips)
            if (s.textured) {
                const uint32_t at = (s.tcw[0] & 0x1FFFFFu) << 3;
                if (at >= 0x100000u && at < 0x800000u && (at & 0xFFF00000u) != 0 && at % 0x1000u == 0)
                    std::fprintf(stderr, "  texture at %06X (TCW %08X)\n", at, s.tcw[0]);
            }
    }
    // QILIN_DUMP_SCENE=<picture>: the first render at or after that picture,
    // strip by strip (list, words, vertex count, x/y extent, 1/w range), to stderr.
    static const long dump_at = std::getenv("QILIN_DUMP_SCENE") ? std::atol(std::getenv("QILIN_DUMP_SCENE")) : -1;
    if (dump_at >= 0 && static_cast<long>(g_pictures) >= dump_at) {
        static bool dumped = false;
        if (!dumped) {
            dumped = true;
            for (const ta::Strip& s : src.scene->strips) {
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
                for (uint32_t k = s.first; k < s.first + s.count; ++k) {
                    const ta::Vertex& v = src.scene->vertices[k];
                    x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y);
                    z0 = std::min(z0, v.z), z1 = std::max(z1, v.z);
                }
                std::fprintf(stderr, "strip list %d pcw %08X isp %08X tsp %08X tcw %08X n %u x %.1f..%.1f y %.1f..%.1f z %g..%g\n",
                             int(s.list), s.pcw, s.isp, s.tsp[0], s.tcw[0], s.count, x0, x1, y0, y1, z0, z1);
            }
        }
    }
    g_renderer->draw(src);
    g_last_drawn = src.frame_buffer;
    g_drawn = true;
}

// The settings menu: once per vertical blank on the window's thread. While
// it's open the game parks here, its time stopped, and the last picture is
// shown again under the menu.
void run_menu() {
    if (!g_menu || std::this_thread::get_id() != g_thread) return;
    g_menu->tick();
    if (!g_menu->wants_pause()) return;
    pause_guest_time(true);
    g_menu->set_parked(true);
    while (g_menu->wants_pause()) {
        if (!g_display->poll()) {
            std::fflush(nullptr);
            std::_Exit(0);
        }
        g_menu->tick();
        if (g_menu->quit_requested()) {
            rt::log("quit from the settings menu");
            std::fflush(nullptr);
            std::_Exit(0);  // guest threads are parked on host threads: no static destructors
        }
        if (!g_menu->wants_pause()) break;
        g_display->present_again();
        if (!g_settings.vsync) SDL_Delay(8);  // without vsync the menu needn't spin
    }
    g_menu->set_parked(false);
    pause_guest_time(false);
}

void (*g_blank_hook)() = nullptr;
void on_blank(void (*hook)()) { g_blank_hook = hook; }
uint64_t pictures_shown() { return g_pictures; }

void vblank() {
    if (g_blank_hook) g_blank_hook();
    if (!g_renderer) return;
    poll();
    measure_screen();
    run_menu();
    // The settings the menu may have changed: each is nothing to do while unchanged.
    g_renderer->set_anisotropy(g_settings.anisotropy);
    g_renderer->set_msaa(g_settings.msaa);
    set_texture_packs();
    if (g_menu && g_menu->take_reload_textures()) g_renderer->reload_texture_packs();
    sound::set_volume(g_settings.volume, g_settings.mute_unfocused && g_display->sdl_window() && !g_display->focused());
    game::set_frame_rate(g_settings.frame_rate, g_settings.frame_rate == kFrameRateDisplay ? g_display->refresh_rate() : 0.0);
    const float strength = g_settings.rumble ? float(g_settings.rumble_strength) / 100.0f : 0.0f;
    for (unsigned port = 0; port < 4; ++port) g_display->rumble(port, strength * maple::vibration(port));
    if (!g_drawn) return;
    // The frame buffer the display reads, if the game has drawn it; else the
    // one drawn last.
    renderer::Picture p = g_renderer->picture(pvr_register(kFbRSof1) & 0x00FFFFFCu);
    if (!p.image) p = g_renderer->picture(g_last_drawn);
    if (!p.image) return;
    g_drawn = false;
    ++g_pictures;
    if (!g_shot_folder.empty() && g_pictures % g_shot_every == 0 && g_pictures >= g_shot_from && g_pictures <= g_shot_to) shot(g_renderer->read_back(), p.width, p.height);
    dump_ram_if_asked();
    // 4:3, widened as it was drawn: never stretched (a movie is never widened).
    g_display->present_image(p.image, p.view, p.width, p.height, 4.0f / 3.0f * p.widen, p.height / g_scale);
}

void show(uint32_t width, uint32_t height, const uint8_t* rgba) {
    if (!g_display) return;
    poll();
    g_display->present(width, height, rgba, 4.0f / 3.0f, height);
}

namespace {
// QILIN_PRESS=<button>@<picture>[,...]: presses a button for ten pictures from
// the given one (button: a, b, x, y, start, up, down, left, right, lt, rt),
// for driving a run without a window; <button>@<first>-<last>/<every>
// presses it every so many pictures, and <button>@<first>+<pictures> holds it.
struct Press {
    uint16_t button;
    bool lt, rt;
    uint64_t from, length = 10;
};
// A whole number at `p`, digits only, with where it ends in `rest`; anything else stops.
uint64_t press_number(const std::string& item, const char* p, const char*& rest) {
    if (*p < '0' || *p > '9') rt::fatal("QILIN_PRESS: %s: a picture number expected at \"%s\"", item.c_str(), p);
    char* e = nullptr;
    const uint64_t v = std::strtoull(p, &e, 10);
    rest = e;
    return v;
}

std::vector<Press> scripted_presses() {
    std::vector<Press> out;
    const char* s = std::getenv("QILIN_PRESS");
    if (!s) return out;
    static constexpr struct {
        const char* name;
        uint16_t button;
    } kNames[] = {{"a", maple::kA},      {"b", maple::kB},      {"x", maple::kX},        {"y", maple::kY},
                  {"start", maple::kStart}, {"up", maple::kUp}, {"down", maple::kDown}, {"left", maple::kLeft},
                  {"right", maple::kRight}, {"lt", 0},          {"rt", 0}};
    const std::string all = s;
    for (size_t at = 0; at < all.size();) {
        const size_t end = std::min(all.find(',', at), all.size());
        const std::string item = all.substr(at, end - at);
        at = end + 1;
        const size_t sign = item.find('@');
        if (sign == std::string::npos) rt::fatal("QILIN_PRESS: %s has no @<picture>", item.c_str());
        const std::string name = item.substr(0, sign);
        const auto* button = std::find_if(std::begin(kNames), std::end(kNames), [&](const auto& n) { return name == n.name; });
        if (button == std::end(kNames)) rt::fatal("QILIN_PRESS: no button called %s", name.c_str());
        const Press press{button->button, name == "lt", name == "rt", 0, 10};
        const char* rest = nullptr;
        const uint64_t first = press_number(item, item.c_str() + sign + 1, rest);
        if (*rest == '+') {  // <first>+<pictures>: held
            Press held = press;
            held.from = first, held.length = press_number(item, rest + 1, rest);
            if (held.length == 0) rt::fatal("QILIN_PRESS: %s: held for no pictures", item.c_str());
            if (*rest) rt::fatal("QILIN_PRESS: %s: something after the pictures held", item.c_str());
            out.push_back(held);
            continue;
        }
        uint64_t last = first, every = 1;
        if (*rest == '-') {  // <first>-<last>/<every>: a press every so many pictures
            last = press_number(item, rest + 1, rest);
            if (*rest != '/') rt::fatal("QILIN_PRESS: %s: <first>-<last>/<every>", item.c_str());
            every = press_number(item, rest + 1, rest);
            if (every < 20) rt::fatal("QILIN_PRESS: %s: presses at least 20 pictures apart", item.c_str());
        }
        if (*rest) rt::fatal("QILIN_PRESS: %s: something after the picture", item.c_str());
        for (uint64_t when = first; when <= last; when += every) {
            Press p = press;
            p.from = when;
            out.push_back(p);
        }
    }
    return out;
}
}  // namespace

namespace {
// The bindings (settings.hpp) with each key's code looked up once, again
// when the menu changes them.
struct Input {
    Binding::Kind kind;
    uint32_t pad;
    int key;
};
std::vector<Input> g_inputs[kControlCount];
bool g_inputs_ready = false;

void look_up_controls() {
    for (unsigned c = 0; c < kControlCount; ++c) {
        g_inputs[c].clear();
        for (const Binding& b : g_settings.controls[c])
            g_inputs[c].push_back({b.kind, b.pad, b.kind == Binding::kKey ? g_display->key_code(b.key.c_str()) : -1});
    }
    g_inputs_ready = true;
}
}  // namespace

// Ports B to D: the host's other pads, through the pad inputs the settings
// bind (the keyboard is port A's).
maple::Controls other_player(unsigned port) {
    maple::Controls c;
    const qilin::host::Display::PadState p = g_display->pad_state(port);
    if (!p.connected || (g_menu && g_menu->is_open())) return c;
    c.connected = true;
    if (!g_inputs_ready) look_up_controls();
    bool on[kControlCount] = {};
    for (unsigned k = 0; k < kControlCount; ++k)
        for (const Input& in : g_inputs[k]) on[k] = on[k] || (in.kind == Binding::kPad && (p.buttons & in.pad));
    static constexpr struct {
        Control control;
        uint16_t dc;
    } kButtons[] = {{kCtlA, maple::kA}, {kCtlB, maple::kB}, {kCtlX, maple::kX}, {kCtlY, maple::kY}, {kCtlStart, maple::kStart},
                    {kCtlUp, maple::kUp}, {kCtlDown, maple::kDown}, {kCtlLeft, maple::kLeft}, {kCtlRight, maple::kRight}};
    for (const auto& b : kButtons)
        if (on[b.control]) c.buttons |= b.dc;
    const auto byte = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    c.lt = on[kCtlL] ? 255 : byte(p.lt), c.rt = on[kCtlR] ? 255 : byte(p.rt);
    c.x = static_cast<uint8_t>(std::lround(std::clamp(p.lx, -1.0f, 1.0f) * 127.5f + 127.5f));
    c.y = static_cast<uint8_t>(std::lround(std::clamp(p.ly, -1.0f, 1.0f) * 127.5f + 127.5f));
    return c;
}

maple::Controls controls(unsigned port) {
    maple::Controls c;
    if (!g_display || port > 3) return c;
    if (port != 0) return other_player(port);
    c.connected = true;  // port A always has a pad: the keyboard if nothing else
    if (g_menu && g_menu->is_open()) return c;  // the menu has the input
    if (!g_inputs_ready || (g_menu && g_menu->take_controls_changed())) look_up_controls();
    const qilin::host::Display::PadState p = g_display->pad_state();
    const uint32_t all = g_display->buttons();
    bool on[kControlCount] = {};
    for (unsigned k = 0; k < kControlCount; ++k)
        for (const Input& in : g_inputs[k])
            on[k] = on[k] || (in.kind == Binding::kPad && (p.buttons & in.pad)) || (in.kind == Binding::kKey && in.key >= 0 && g_display->key_down(in.key)) ||
                    (in.kind == Binding::kMouse && (all & qilin::host::kPadClick));
    static constexpr struct {
        Control control;
        uint16_t dc;
    } kButtons[] = {{kCtlA, maple::kA}, {kCtlB, maple::kB}, {kCtlX, maple::kX}, {kCtlY, maple::kY}, {kCtlStart, maple::kStart},
                    {kCtlUp, maple::kUp}, {kCtlDown, maple::kDown}, {kCtlLeft, maple::kLeft}, {kCtlRight, maple::kRight}};
    for (const auto& b : kButtons)
        if (on[b.control]) c.buttons |= b.dc;
    // The pad's stick and triggers in full; a bound input all the way.
    const auto byte = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    c.lt = on[kCtlL] ? 255 : byte(p.lt), c.rt = on[kCtlR] ? 255 : byte(p.rt);
    c.x = static_cast<uint8_t>(std::lround(std::clamp(p.lx, -1.0f, 1.0f) * 127.5f + 127.5f));
    c.y = static_cast<uint8_t>(std::lround(std::clamp(p.ly, -1.0f, 1.0f) * 127.5f + 127.5f));
    if (on[kCtlStickLeft] != on[kCtlStickRight]) c.x = on[kCtlStickLeft] ? 0 : 255;
    if (on[kCtlStickUp] != on[kCtlStickDown]) c.y = on[kCtlStickUp] ? 0 : 255;
    static const std::vector<Press> presses = scripted_presses();
    for (const Press& p2 : presses)
        if (g_pictures >= p2.from && g_pictures < p2.from + p2.length) {
            c.buttons |= p2.button;
            if (p2.lt) c.lt = 255;
            if (p2.rt) c.rt = 255;
        }
    return c;
}

void poll() {
    if (!g_display || std::this_thread::get_id() != g_thread) return;
    if (!g_display->poll()) {
        std::fflush(nullptr);
        std::_Exit(0);
    }
}

}  // namespace dreamcast::video
