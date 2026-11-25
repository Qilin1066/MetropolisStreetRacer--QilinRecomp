// The game's frame rate (docs/runtime.md, "Frame rate").
//
// The game draws a picture every vertical blank in its menus and every second one in a race. Two
// setters choose (called through a table of the game's): 0x8C02D0D0 for every blank, 0x8C02D0E6
// for every second one. Each stores the blanks a picture at 0x8C1AC2B4, which the game's wait for
// the next picture goes by, and the pictures a second at 0x8C1BB95C: the display's refresh, the
// float at 0x8C1BB960 (60.0), divided by the blanks. The game's timing reads that float, in about
// 80 places, so a race drawn every blank runs at the same speed. Its race clock is the exception:
// a whole number of milliseconds a picture, trunc(1000 / pictures a second), 33 at 30 a second but
// 16 at 60, which would run the clock 4% slow. Those steps are lifted as rt::frame_step
// (KnownBuild::frame_steps), told here how many pictures are drawn for each one the game asked
// for, so the clock keeps the game's sums.
//
// The cars' physics is the other exception. It keeps velocities per picture and is stepped in
// pictures: its suspension puts the wheels back on the road a fraction of the way a picture, its
// engine and tyres smooth and clamp per picture, and the race's world step works out each car's
// velocity from how far it moved since the last picture. Stepped twice as often it drives
// differently (the cars pulled away faster at 60), and no rescaling of its constants makes a
// solver like that match. So it runs as the game ran it: the world step (0x8C03BAD4, a task once a
// picture: every car's step, their collisions) every second picture, at the game's frame rate,
// from exactly the state it would have had. It runs half a picture early, on the picture before
// the one the game would have run it for, and that picture shows each car halfway between its
// last pose and its new one; the next picture puts the new pose back, as the game left it, and
// shows that. What reads a car between (the camera, the renderer, the race's progress) sees it
// where it is at that moment. A car's per-picture values (its velocity, rotation and wheel spins)
// are kept in the port's units outside the world step, as the rest of the game reads them (the
// speedometer, the camera, the sound), and put back in the game's for it.
//
// Both setters are native: they keep what the game asked for and store what the player chose.
// The choice can change at any vertical blank (the settings menu); the game's values are then
// written again.
//
// Runtime code: no std::format.
#include "frame_rate.hpp"

#include <array>
#include <cstdint>

#include "game.hpp"
#include "guest.hpp"
#include "system.hpp"

namespace msr {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kBlanksPerPicture = 0x8C1AC2B4u;
constexpr uint32_t kPicturesPerSecond = 0x8C1BB95Cu;  // float
constexpr uint32_t kRefresh = 0x8C1BB960u;            // float: the display's, 60.0 here (the console's NTSC)

uint32_t g_asked = 0;  // the blanks a picture the game last asked for (0: it hasn't yet)
unsigned g_rate = 0;   // the player's choice: 0, the game's own; 60

float game_rate() { return rt::f32(rt::ld32(kRefresh)) / static_cast<float>(g_asked); }

void store() {
    const uint32_t blanks = g_rate == 60 ? 1u : g_asked;
    const float refresh = rt::f32(rt::ld32(kRefresh));
    if (!(refresh > 0.0f)) rt::fatal("msr: the game's display refresh (0x%08X) is %g", kRefresh, double(refresh));
    rt::st32(kBlanksPerPicture, blanks);
    rt::st32(kPicturesPerSecond, rt::b32(refresh / static_cast<float>(blanks)));
    rt::g_frame_split = g_asked / blanks;
}

void ask(uint32_t blanks) {
    g_asked = blanks;
    store();
}

void every_blank(rt::Context&) { ask(1); }
void every_second_blank(rt::Context&) { ask(2); }

constexpr std::array<rt::Provided, 2> kNatives{{
    {0x8C02D0D0u, "MSR: a picture every vertical blank", every_blank},
    {0x8C02D0E6u, "MSR: a picture every second vertical blank", every_second_blank},
}};

// A body at the game's own frame rate: the pictures-a-second float and the shared-out steps as
// it asked, as the original ran it.
void at_game_rate(rt::Context& c, rt::Body body) {
    const uint32_t fps = rt::ld32(kPicturesPerSecond);
    const unsigned split = rt::g_frame_split;
    rt::st32(kPicturesPerSecond, rt::b32(game_rate()));
    rt::g_frame_split = 1;
    body(c);
    rt::st32(kPicturesPerSecond, fps);
    rt::g_frame_split = split;
}

// A car's pose: its matrix, four rows of four floats from +0 (three axes and the position, the
// fourth column padding). Halfway between two poses a 30th of a second apart is near enough a pose
// for the picture between.
constexpr uint32_t kPoseFloats = 16;
struct Car {
    uint32_t at = 0;
    uint32_t before[kPoseFloats] = {};  // its pose as the world step began
    uint32_t after[kPoseFloats] = {};   // its new pose, put back at the next picture
    float wheels_before[4] = {};         // its wheels' angles as the world step began
    float wheels_after[4] = {};          // and after
};
std::array<Car, 16> g_cars{};
// A car's words kept per picture, read outside the physics: velocity, rotation, each wheel's spin
// (108-byte wheels from +5C, spin at +24 of each), each wheel's speed over the road (+2F4) and the
// velocity the update started from (+3A0). In the game's units inside the world step, the port's
// outside.
constexpr uint32_t kPerPicture[] = {0x040, 0x044, 0x048, 0x04C, 0x050, 0x054, 0x080, 0x0EC, 0x158, 0x1C4,
                                    0x2F4, 0x2F8, 0x2FC, 0x300, 0x3A0, 0x3A4, 0x3A8};
// Each wheel's angle (+20 of each), turned by its spin, between -10 pi and 10 pi.
constexpr uint32_t kWheelAngles[] = {0x07C, 0x0E8, 0x154, 0x1C0};
constexpr float kWheelTurn = 62.831853f;
size_t g_car_count = 0;
bool g_in_world_step = false;  // the world step is running (its cars' steps record them)
bool g_between = false;        // the last picture ran the world step and left the cars halfway

void pose(uint32_t car, uint32_t* to) {
    for (uint32_t i = 0; i < kPoseFloats; ++i) to[i] = rt::ld32(car + 4 * i);
}
void set_pose(uint32_t car, const uint32_t* from) {
    for (uint32_t i = 0; i < kPoseFloats; ++i) rt::st32(car + 4 * i, from[i]);
}
void scale_per_picture(uint32_t car, float k) {
    for (const uint32_t off : kPerPicture) rt::st32(car + off, rt::b32(rt::f32(rt::ld32(car + off)) * k));
}

void world_step(rt::Context& c, rt::Body body) {
    if (rt::g_frame_split <= 1) {
        // Back at the game's rate (the choice changed): the cars stepped last, in its units again.
        if (g_between)
            for (size_t i = 0; i < g_car_count; ++i) set_pose(g_cars[i].at, g_cars[i].after);
        for (size_t i = 0; i < g_car_count; ++i) scale_per_picture(g_cars[i].at, 2.0f);
        g_car_count = 0;
        g_between = false;
        return body(c);
    }
    if (rt::g_frame_split != 2) rt::fatal("msr: the race's world step at %u pictures for each of the game's", rt::g_frame_split);
    if (g_between) {  // the picture the game ran the world step for: each car's new pose, as it left it
        for (size_t i = 0; i < g_car_count; ++i) {
            const Car& car = g_cars[i];
            set_pose(car.at, car.after);
            for (size_t w = 0; w < 4; ++w) rt::st32(car.at + kWheelAngles[w], rt::b32(car.wheels_after[w]));
        }
        g_between = false;
        return;
    }
    // The cars it stepped last time: their per-picture values back in the game's units.
    for (size_t i = 0; i < g_car_count; ++i) scale_per_picture(g_cars[i].at, 2.0f);
    g_car_count = 0;
    g_in_world_step = true;
    at_game_rate(c, body);
    g_in_world_step = false;
    for (size_t i = 0; i < g_car_count; ++i) {
        Car& car = g_cars[i];
        pose(car.at, car.after);
        for (uint32_t k = 0; k < kPoseFloats; ++k)
            rt::st32(car.at + 4 * k, rt::b32(0.5f * (rt::f32(car.before[k]) + rt::f32(car.after[k]))));
        for (size_t w = 0; w < 4; ++w) {  // halfway round, the short way across the wrap
            car.wheels_after[w] = rt::f32(rt::ld32(car.at + kWheelAngles[w]));
            float turn = car.wheels_after[w] - car.wheels_before[w];
            if (turn > 0.5f * kWheelTurn) turn -= kWheelTurn;
            if (turn < -0.5f * kWheelTurn) turn += kWheelTurn;
            rt::st32(car.at + kWheelAngles[w], rt::b32(car.wheels_before[w] + 0.5f * turn));
        }
        scale_per_picture(car.at, 0.5f);
    }
    g_between = true;
}

// A car's step: inside the world step, the car is noted (with its pose before) for the picture
// between. Anywhere else, at the game's rate.
void car_step(rt::Context& c, rt::Body body) {
    if (!g_in_world_step) return rt::g_frame_split <= 1 ? body(c) : at_game_rate(c, body);
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

// A car's update on its own (the start's settling runs call it): at the game's rate.
void car_update(rt::Context& c, rt::Body body) {
    if (g_in_world_step || rt::g_frame_split <= 1) return body(c);
    at_game_rate(c, body);
}

// The race's setup (a task: the cars put on the grid and settled, their updates run in loops):
// at the game's rate throughout, as the original settled them.
void race_setup(rt::Context& c, rt::Body body) {
    if (rt::g_frame_split <= 1) return body(c);
    at_game_rate(c, body);
}

constexpr std::array<rt::Wrapper, 4> kWrappers{{
    {0x8C03B3E2u, "MSR: the race's setup, at the game's own frame rate", race_setup},
    {0x8C03BAD4u, "MSR: the race's world step, at the game's own frame rate", world_step},
    {0x8C03C14Eu, "MSR: a car's step in the race", car_step},
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

void set_frame_rate(unsigned rate) {
    if (rate == msr::g_rate) return;
    msr::g_rate = rate;
    if (msr::g_asked) msr::store();
}

}  // namespace dreamcast::game
