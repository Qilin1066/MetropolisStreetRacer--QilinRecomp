// The game's frame rate (docs/runtime.md, "Frame rate").
//
// The game draws a picture every vertical blank in its menus and every second one in a race. Two
// setters choose (called through a table of the game's): 0x8C02D0D0 for every blank, 0x8C02D0E6
// for every second one. Each stores the blanks a picture at 0x8C1AC2B4, which the game's wait for
// the next picture goes by, and the pictures a second at 0x8C1BB95C: the display's refresh, the
// float at 0x8C1BB960 (60.0), divided by the blanks. Both are native: they keep what the game
// asked for, and in a race the player's choice (frame_rate: a number, the display's refresh, or
// unlimited) sets the vertical blank's rate (dreamcast::set_blank_rate), a picture every blank,
// and that float. The menus keep the game's own rate.
//
// The game's timing reads that float in about 80 places, so a race drawn at any rate runs at the
// same speed. Its race clock is an exception: a whole number of milliseconds a picture,
// trunc(1000 / pictures a second), 33 at 30 a second but 16 at 60, which would run the clock 4%
// slow. Those steps are lifted as rt::frame_step (KnownBuild::frame_steps), told here what share
// of one of the game's pictures each of the port's is (rt::g_frame_share), so the clock keeps the
// game's sums.
//
// The cars' physics is the other exception. It keeps velocities per picture and is stepped in
// pictures: its suspension puts the wheels back on the road a fraction of the way a picture, its
// engine and tyres smooth and clamp per picture, and the race's world step works out each car's
// velocity from how far it moved since the last picture. Stepped more often it drives differently
// (the cars pulled away faster at 60), and no rescaling of its constants makes a solver like that
// match. So it runs as the game ran it: the world step (0x8C03BAD4, a task once a picture: every
// car's step, their collisions) at the game's frame rate, from exactly the state it would have
// had, as often as the game ran it (30 a second): each time the time shown passes the last state
// it worked out, a little early, so that every picture shows each car where it is at that moment,
// between its last two states. What reads a car between (the camera, the renderer, the race's
// progress) sees it there. A car's per-picture values (its velocity, rotation and wheel spins)
// are kept in the port's units outside the world step, as the rest of the game reads them (the
// speedometer, the camera, the sound), and put back in the game's for it.
//
// The choice can change at any vertical blank (the settings menu); the game's values are then
// written again.
//
// Runtime code: no std::format.
#include "frame_rate.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "clock.hpp"
#include "game.hpp"
#include "guest.hpp"
#include "interrupts.hpp"
#include "settings.hpp"
#include "system.hpp"

namespace msr {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kBlanksPerPicture = 0x8C1AC2B4u;
constexpr uint32_t kPicturesPerSecond = 0x8C1BB95Cu;  // float
constexpr uint32_t kRefresh = 0x8C1BB960u;            // float: the display's, 60.0 here (the console's NTSC)
constexpr double kMinRate = 30.0, kMaxRate = dreamcast::kFrameRateMax;

uint32_t g_asked = 0;  // the blanks a picture the game last asked for (0: it hasn't yet)
unsigned g_rate = dreamcast::kFrameRateOriginal;  // the player's choice (Settings::frame_rate)
double g_display_hz = 0;                          // the display's refresh, for kFrameRateDisplay
double g_unlimited_rate = 60;                     // unlimited: the rate the last picture went at
// A race is drawn at the chosen rate only once the player's car is loaded: the drive's stream
// server moves a piece on each picture, so drawn faster the load would end sooner in the game's
// pictures than the original's, and its count of pictures (the seconds the physics reads) differ.
// The car's model pointer (+4C0 of the player's car, 0x8C51F79C) is 0 until then.
constexpr uint32_t kPlayerModel = 0x8C51F79Cu + 0x4C0u;  // +4C0 of the player's car
enum class Load { gone, empty, loaded };  // the model as last seen since the game asked: not yet seen 0, 0, set
Load g_load = Load::gone;

float refresh() { return rt::f32(rt::ld32(kRefresh)); }
float game_rate() { return refresh() / static_cast<float>(g_asked); }

// The port's pictures a second in a race, or 0 for the game's own.
double chosen_rate() {
    switch (g_rate) {
    case dreamcast::kFrameRateOriginal: return 0;
    case dreamcast::kFrameRateDisplay: return g_display_hz >= kMinRate ? g_display_hz : 60.0;
    case dreamcast::kFrameRateUnlimited: return g_unlimited_rate;
    default: return g_rate;
    }
}

void set_rate(float pictures_per_second) {
    rt::st32(kPicturesPerSecond, rt::b32(pictures_per_second));
    rt::g_frame_share = game_rate() / pictures_per_second;
}

void store() {
    if (!(refresh() > 0.0f)) rt::fatal("msr: the game's display refresh (0x%08X) is %g", kRefresh, double(refresh()));
    const double rate = g_asked >= 2 && g_load == Load::loaded ? chosen_rate() : 0;
    if (rate > game_rate()) {  // a race drawn faster than the game's own rate: a picture every blank
        rt::st32(kBlanksPerPicture, 1);
        dreamcast::set_blank_rate(g_rate == dreamcast::kFrameRateUnlimited ? 0.0 : rate);
        set_rate(static_cast<float>(rate));
        return;
    }
    rt::st32(kBlanksPerPicture, g_asked);
    dreamcast::set_blank_rate(refresh());
    rt::st32(kPicturesPerSecond, rt::b32(game_rate()));
    rt::g_frame_share = 1.0;
}

void ask(uint32_t blanks) {
    g_asked = blanks;
    g_load = Load::gone;
    store();
}

// Once a picture: has the player's car loaded since the game asked for its rate?
void watch_load() {
    if (g_load == Load::loaded || g_asked < 2) return;
    const bool set = rt::ld32(kPlayerModel) != 0;
    if (g_load == Load::gone && !set) g_load = Load::empty;
    else if (g_load == Load::empty && set) g_load = Load::loaded, store();
}

void every_blank(rt::Context&) { ask(1); }
void every_second_blank(rt::Context&) { ask(2); }

// The game's timeline while it's drawn faster than it asked: in its own pictures, moved on once a
// picture by the task runner (0x8C02CBBC, which runs the game's tasks in order once a picture) by
// the guest time since the last, at most one of the game's pictures (a slower picture slows the
// game, as the original's did, rather than catching up). The game's pictures that fall due in a
// picture (whole ones passed) are how many times the tasks kept at the game's rate run in it.
double g_time = 0;        // in the game's pictures
unsigned g_due = 0;       // the game's pictures due in this picture
uint64_t g_frame_ns = 0;  // when the last picture began (guest time)

double whole_ceil(double t) { return std::ceil(t - 1e-9); }

// The task that counts the game's pictures (0x8C098278, once a picture): unless paused (the word at
// 0x8C53B380), one more picture in the float at 0x8C54571C and the seconds, that count divided by
// the pictures a second, at 0x8C545718; then the tasks of chain 9 (0x8C02CF0C, r4 9). Parts of
// the race (the cars' physics among them) read those seconds. Drawn faster than the game asked, it
// counts the game's pictures due (the timeline), its seconds at the game's rate; chain 9 still
// runs every picture.
constexpr uint32_t kPaused = 0x8C53B380u, kPictureCount = 0x8C54571Cu, kSeconds = 0x8C545718u;
void count_one(uint32_t rate) {
    if (rt::ld32(kPaused) != 0) return;
    const uint32_t count = rt::fadd(rt::ld32(kPictureCount), 0x3F800000u);
    rt::st32(kPictureCount, count);
    rt::st32(kSeconds, rt::fdiv(count, rate));
}
void count_pictures(rt::Context& c) {
    if (rt::g_frame_share >= 1.0) count_one(rt::ld32(kPicturesPerSecond));
    else
        for (unsigned i = 0; i < g_due; ++i) count_one(rt::b32(game_rate()));
    c.r[4] = 9;
    rt::call_guest(c, 0x8C02CF0Cu);
}

constexpr std::array<rt::Provided, 3> kNatives{{
    {0x8C02D0D0u, "MSR: a picture every vertical blank", every_blank},
    {0x8C02D0E6u, "MSR: a picture every second vertical blank", every_second_blank},
    {0x8C098278u, "MSR: the count of the game's pictures and seconds", count_pictures},
}};

// A body at the game's own frame rate: the pictures-a-second float and the shared-out steps as
// it asked, as the original ran it.
void at_game_rate(rt::Context& c, rt::Body body) {
    const uint32_t fps = rt::ld32(kPicturesPerSecond);
    const double share = rt::g_frame_share;
    rt::st32(kPicturesPerSecond, rt::b32(game_rate()));
    rt::g_frame_share = 1.0;
    body(c);
    rt::st32(kPicturesPerSecond, fps);
    rt::g_frame_share = share;
}

// A car's pose: its matrix, four rows of four floats from +0 (three axes and the position, the
// fourth column padding). Between two poses a 30th of a second apart, a straight line is near
// enough a pose for the pictures between.
constexpr uint32_t kPoseFloats = 16;
// A car's words kept per picture, read outside the physics: velocity, rotation, each wheel's spin
// (108-byte wheels from +5C, spin at +24 of each), each wheel's speed over the road (+2F4) and the
// velocity the update started from (+3A0). In the game's units inside the world step, the port's
// outside. The values the physics left are kept, and put back exactly where nothing outside has
// changed them since (30 / 144 and back isn't exact in floating point).
constexpr uint32_t kPerPicture[] = {0x040, 0x044, 0x048, 0x04C, 0x050, 0x054, 0x080, 0x0EC, 0x158, 0x1C4,
                                    0x2F4, 0x2F8, 0x2FC, 0x300, 0x3A0, 0x3A4, 0x3A8};
constexpr size_t kPerPictureCount = std::size(kPerPicture);
// Each wheel's angle (+20 of each), turned by its spin, between -10 pi and 10 pi.
constexpr uint32_t kWheelAngles[] = {0x07C, 0x0E8, 0x154, 0x1C0};
constexpr float kWheelTurn = 62.831853f;

struct Car {
    uint32_t at = 0;
    uint32_t before[kPoseFloats] = {};  // its pose as the last world step began
    uint32_t after[kPoseFloats] = {};   // its pose as it left it: the true one
    float wheels_before[4] = {};
    float wheels_after[4] = {};
    uint32_t per_picture[kPerPictureCount] = {};  // as the physics left them (the game's units)
    uint32_t written[kPerPictureCount] = {};      // as last written in the port's
};
std::array<Car, 16> g_cars{};
size_t g_car_count = 0;
bool g_in_world_step = false;  // the world step is running (its cars' steps record them)
bool g_stepped = false;        // the cars are shown between their last two states (g_cars)
bool g_corners_due = false;    // a world step has run since the corners task last did (corners)
double g_units = 1;            // the cars' per-picture values are in the game's units times this

// QILIN_PHYSICS_LOG=<file> (development): after each world step, a line: its number, the player's
// throttle, a hash of the player's car (0x8C51F79C, 2 KB) as the physics left it, and the game's
// picture count and seconds; and the car itself, 2 KB a step, in <file>.bin. Two runs driven the
// same way at different frame rates give the same lines: the test that the physics is the game's at
// any rate (docs/runtime.md, "Frame rate").
constexpr uint32_t kPlayer = 0x8C51F79Cu;
void log_step() {
    static std::FILE* const out = [] {
        const char* path = std::getenv("QILIN_PHYSICS_LOG");
        return path ? std::fopen(path, "w") : nullptr;
    }();
    if (!out) return;
    static std::FILE* const raw = std::fopen((std::string(std::getenv("QILIN_PHYSICS_LOG")) + ".bin").c_str(), "wb");
    if (!raw) rt::fatal("msr: cannot write %s.bin", std::getenv("QILIN_PHYSICS_LOG"));
    static uint64_t step = 0;
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t off = 0; off < 0x800; off += 4)
        if (off != 0x4FC && off != 0x504)  // a running total the renderer keeps there, not the physics
            hash = (hash ^ rt::ld32(kPlayer + off)) * 1099511628211ull;
    std::fprintf(out, "%llu %g %016llx %.9g %.9g\n", static_cast<unsigned long long>(step++), double(rt::f32(rt::ld32(kPlayer + 0x324))),
                 static_cast<unsigned long long>(hash), double(rt::f32(rt::ld32(kPictureCount))), double(rt::f32(rt::ld32(kSeconds))));
    std::fflush(out);
    if (const uint8_t* car = rt::ram_range(kPlayer, 0x800)) std::fwrite(car, 1, 0x800, raw);
}

void pose(uint32_t car, uint32_t* to) {
    for (uint32_t i = 0; i < kPoseFloats; ++i) to[i] = rt::ld32(car + 4 * i);
}
void set_pose(uint32_t car, const uint32_t* from) {
    for (uint32_t i = 0; i < kPoseFloats; ++i) rt::st32(car + 4 * i, from[i]);
}
// A car's per-picture values as the physics left them, its true ones (`units` 1), or in the port's
// units (`units` the share). One something outside has changed since it was written is taken as
// it is, in the units it was written in.
void per_picture_in(Car& car, double units) {
    for (size_t i = 0; i < kPerPictureCount; ++i) {
        const uint32_t at = car.at + kPerPicture[i], now = rt::ld32(at);
        if (now != car.written[i]) car.per_picture[i] = rt::b32(static_cast<float>(rt::f32(now) / g_units));
        const uint32_t v = units == 1.0 ? car.per_picture[i] : rt::b32(static_cast<float>(rt::f32(car.per_picture[i]) * units));
        rt::st32(at, v);
        car.written[i] = v;
    }
}

// The cars as the physics left them: true poses and the game's units.
void put_back() {
    if (!g_stepped) return;
    for (size_t i = 0; i < g_car_count; ++i) {
        Car& car = g_cars[i];
        set_pose(car.at, car.after);
        for (size_t w = 0; w < 4; ++w) rt::st32(car.at + kWheelAngles[w], rt::b32(car.wheels_after[w]));
        per_picture_in(car, 1.0);
    }
    g_units = 1;
}

// Each car `t` of the way from its pose before the last world step to the one after (t 1: exactly
// the latter), in the port's units.
void show(double t) {
    const double units = rt::g_frame_share;
    for (size_t i = 0; i < g_car_count; ++i) {
        Car& car = g_cars[i];
        per_picture_in(car, units);
        if (t >= 1.0) {
            set_pose(car.at, car.after);
            for (size_t w = 0; w < 4; ++w) rt::st32(car.at + kWheelAngles[w], rt::b32(car.wheels_after[w]));
            continue;
        }
        const float f = static_cast<float>(t);
        for (uint32_t k = 0; k < kPoseFloats; ++k) {
            const float a = rt::f32(car.before[k]), b = rt::f32(car.after[k]);
            rt::st32(car.at + 4 * k, rt::b32(a + (b - a) * f));
        }
        for (size_t w = 0; w < 4; ++w) {  // the short way across the wrap
            float turn = car.wheels_after[w] - car.wheels_before[w];
            if (turn > 0.5f * kWheelTurn) turn -= kWheelTurn;
            if (turn < -0.5f * kWheelTurn) turn += kWheelTurn;
            rt::st32(car.at + kWheelAngles[w], rt::b32(car.wheels_before[w] + turn * f));
        }
    }
    g_units = units;
}

// How far the time shown is from the car states before the last world step to those after (1:
// exactly the latter).
double shown() { return 1.0 - (whole_ceil(g_time) - g_time); }

void world_step(rt::Context& c, rt::Body body) {
    if (rt::g_frame_share >= 1.0) {  // the game's own rate (or the choice changed back): as it ran
        put_back();
        g_stepped = false;
        g_car_count = 0;
        body(c);
        log_step();
        return;
    }
    // Each of the game's pictures due: the next state worked out from the last, at its rate.
    for (unsigned i = 0; i < g_due; ++i) {
        put_back();
        g_car_count = 0;
        g_in_world_step = true;
        at_game_rate(c, body);
        g_in_world_step = false;
        log_step();
        for (size_t k = 0; k < g_car_count; ++k) {
            Car& car = g_cars[k];
            pose(car.at, car.after);
            for (size_t w = 0; w < 4; ++w) car.wheels_after[w] = rt::f32(rt::ld32(car.at + kWheelAngles[w]));
            for (size_t j = 0; j < kPerPictureCount; ++j) car.written[j] = car.per_picture[j] = rt::ld32(car.at + kPerPicture[j]);
        }
        g_units = 1;
        g_stepped = true;
        g_corners_due = true;
    }
    if (g_stepped) show(shown());
}

// The task runner (0x8C02CBBC): the game's timeline moved on, then its tasks.
void task_runner(rt::Context& c, rt::Body body) {
    watch_load();
    const uint64_t now = dreamcast::guest_ns();
    if (rt::g_frame_share >= 1.0) {
        g_time = 0;
        g_due = 1;
        g_frame_ns = 0;
        return body(c);
    }
    const double games = g_frame_ns && now > g_frame_ns ? static_cast<double>(now - g_frame_ns) * game_rate() / 1e9 : rt::g_frame_share;
    const double before = g_time;
    g_time += std::min(games, 1.0);
    g_due = static_cast<unsigned>(whole_ceil(g_time) - whole_ceil(before));
    if (g_time > 1e6) g_time -= std::floor(g_time) - 1.0;  // keep the fraction (and one whole) only
    g_frame_ns = now;
    if (g_rate == dreamcast::kFrameRateUnlimited && games > 0) {  // the next picture's rate: this one's
        g_unlimited_rate = std::clamp(game_rate() / games, game_rate() * 1.0, kMaxRate);
        set_rate(static_cast<float>(g_unlimited_rate));
    }
    body(c);
}

// A task kept at the game's rate: run once for each of the game's pictures due, at its rate.
void at_game_pace(rt::Context& c, rt::Body body) {
    if (rt::g_frame_share >= 1.0) return body(c);
    for (unsigned i = 0; i < g_due; ++i) at_game_rate(c, body);
}

// The task after the world step (0x8C03DCBC): every car's wheel and corner positions from its pose,
// and counters on its wheels, which the next world step reads. The game ran it once a picture
// after the world step, on the poses it left: here, once after each world step, on those poses
// (put back for it, then the picture's shown again). Skipped on the pictures between, so what the
// physics reads is what the original's did.
void corners(rt::Context& c, rt::Body body) {
    if (rt::g_frame_share >= 1.0 || !g_stepped) return body(c);
    if (!g_corners_due) return;
    g_corners_due = false;
    put_back();
    at_game_rate(c, body);
    show(shown());
}

// A car's step: inside the world step, the car is noted (with its pose before) for the pictures
// between. Anywhere else, at the game's rate.
void car_step(rt::Context& c, rt::Body body) {
    if (!g_in_world_step) return rt::g_frame_share >= 1.0 ? body(c) : at_game_rate(c, body);
    const uint32_t at = c.r[4];
    bool seen = false;
    for (size_t i = 0; i < g_car_count; ++i) seen |= g_cars[i].at == at;
    if (!seen) {
        if (g_car_count == g_cars.size()) rt::fatal("msr: more than %zu cars in the race's world step", g_cars.size());
        Car& car = g_cars[g_car_count++];
        car.at = at;
        pose(at, car.before);
        for (size_t w = 0; w < 4; ++w) car.wheels_before[w] = rt::f32(rt::ld32(at + kWheelAngles[w]));
    }
    body(c);
}

// A car's update on its own (the race's setup calls it): at the game's rate.
void car_update(rt::Context& c, rt::Body body) {
    if (g_in_world_step || rt::g_frame_share >= 1.0) return body(c);
    at_game_rate(c, body);
}

// The race's setup (a task: the cars put on the grid and settled, their updates run in loops):
// at the game's rate throughout, as the original settled them.
void race_setup(rt::Context& c, rt::Body body) {
    if (rt::g_frame_share >= 1.0) return body(c);
    at_game_rate(c, body);
}

constexpr std::array<rt::Wrapper, 24> kWrappers{{
    {0x8C02CBBCu, "MSR: the task runner, the game's timeline", task_runner},
    {0x8C074204u, "MSR: the race's presentation (PRESENTATION), at the game's rate", at_game_pace},
    {0x8C03E6F8u, "MSR: the race's animated text (ANIM_TEXT), at the game's rate", at_game_pace},
    {0x8C03ECDCu, "MSR: birds (BIRDS), at the game's rate", at_game_pace},
    {0x8C03F818u, "MSR: boats (BOAT), at the game's rate", at_game_pace},
    {0x8C03FF9Cu, "MSR: the helicopter (CHOPPER), at the game's rate", at_game_pace},
    {0x8C041A90u, "MSR: cones (CONES), at the game's rate", at_game_pace},
    {0x8C04334Au, "MSR: flags (FLAGS), at the game's rate", at_game_pace},
    {0x8C04597Eu, "MSR: fog (FOGSETUP), at the game's rate", at_game_pace},
    {0x8C04AAF2u, "MSR: particles (PARTICLE), at the game's rate", at_game_pace},
    {0x8C04B660u, "MSR: the plane (PLANE), at the game's rate", at_game_pace},
    {0x8C04C7BCu, "MSR: raindrops (RAINDROPS), at the game's rate", at_game_pace},
    {0x8C04DA68u, "MSR: seagulls (SEAGULL), at the game's rate", at_game_pace},
    {0x8C04E8F4u, "MSR: sparks (SPARKS), at the game's rate", at_game_pace},
    {0x8C04EBFEu, "MSR: spray (SPRAY), at the game's rate", at_game_pace},
    {0x8C050D18u, "MSR: stop zones (STOPZONES), at the game's rate", at_game_pace},
    {0x8C0511E6u, "MSR: traffic (TRAFFIC), at the game's rate", at_game_pace},
    {0x8C0516CCu, "MSR: the train (TRAIN), at the game's rate", at_game_pace},
    {0x8C07660Cu, "MSR: the landscape (its sky's turn) (LANDSCAPE), at the game's rate", at_game_pace},
    {0x8C03B3E2u, "MSR: the race's setup, at the game's own frame rate", race_setup},
    {0x8C03BAD4u, "MSR: the race's world step, at the game's own frame rate", world_step},
    {0x8C03C14Eu, "MSR: a car's step in the race", car_step},
    {0x8C03DCBCu, "MSR: the cars' corners after the world step, at its rate", corners},
    {0x8C0AB7B8u, "MSR: a car's update, at the game's own frame rate", car_update},
}};

}  // namespace

void install_frame_rate() {
    rt::provide(kNatives);
    rt::provide_wrappers(kWrappers);
}

}  // namespace msr

namespace dreamcast::game {

bool frame_rate_choice() { return true; }

void set_frame_rate(unsigned rate, double display_hz) {
    if (rate == msr::g_rate && (rate != kFrameRateDisplay || display_hz == msr::g_display_hz)) return;
    msr::g_rate = rate;
    msr::g_display_hz = display_hz;
    msr::g_frame_ns = 0;
    if (msr::g_asked) msr::store();
}

}  // namespace dreamcast::game
