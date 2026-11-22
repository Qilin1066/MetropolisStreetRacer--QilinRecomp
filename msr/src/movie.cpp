// The intro movie: movie.hpp.
#include "movie.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "adx.hpp"
#include "maple.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>  // pl_mpeg.h uses size_t and FILE without including them

#include "pl_mpeg.h"
#include "qilin/host/audio.hpp"
#include "system.hpp"
#include "video.hpp"

namespace msr {

namespace rt = qilin::rt;

namespace {

// The sound: stream 0xC0's packets, in order, are one ADX stream.
Pcm movie_sound(std::vector<uint8_t>& sfd) {
    plm_buffer_t* buffer = plm_buffer_create_with_memory(sfd.data(), sfd.size(), 0);
    plm_demux_t* demux = plm_demux_create(buffer, 1);
    std::vector<uint8_t> adx;
    while (plm_packet_t* p = plm_demux_decode(demux))
        if (p->type == PLM_DEMUX_PACKET_AUDIO_1) adx.insert(adx.end(), p->data, p->data + p->length);
    plm_demux_destroy(demux);
    Pcm pcm;
    std::string why;
    if (!decode_adx(adx, pcm, why)) rt::fatal("msr: the intro movie's sound: %s", why.c_str());
    return pcm;
}

bool skip_pressed(uint16_t& held) {
    constexpr uint16_t kSkip = dreamcast::maple::kA | dreamcast::maple::kB | dreamcast::maple::kX | dreamcast::maple::kY |
                               dreamcast::maple::kStart;
    const uint16_t now = dreamcast::video::controls(0).buttons & kSkip;
    const bool pressed = now & ~held;  // a press, not a button held from before
    held = now;
    return pressed;
}

}  // namespace

void play_intro(std::vector<uint8_t> sfd) {
    if (const char* h = std::getenv("QILIN_HEADLESS"); h && *h && *h != '0') {
        rt::log("msr: headless: the intro movie isn't shown");
        return;
    }
    const Pcm sound = movie_sound(sfd);
    plm_t* plm = plm_create_with_memory(sfd.data(), sfd.size(), 0);
    if (!plm || !plm_probe(plm, 5000 * 1024)) rt::fatal("msr: the intro movie (MSR.SFD) has no MPEG-1 video pl_mpeg can read");
    plm_set_audio_enabled(plm, 0);
    const auto width = static_cast<uint32_t>(plm_get_width(plm)), height = static_cast<uint32_t>(plm_get_height(plm));
    if (width == 0 || height == 0 || width > 1024 || height > 1024) rt::fatal("msr: the intro movie's picture is %ux%u", width, height);
    std::vector<uint8_t> rgba(size_t(width) * height * 4);

    qilin::host::AudioOut out;
    const size_t frames = sound.left_right.size() / 2;
    size_t pushed = 0;
    uint16_t held = 0;
    skip_pressed(held);
    using clock = std::chrono::steady_clock;
    const clock::time_point start = clock::now();
    while (plm_frame_t* frame = plm_decode_video(plm)) {
        const auto due = start + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(frame->time));
        // The sound, up to a tenth of a second ahead of the picture.
        const auto until = std::min(frames, static_cast<size_t>((frame->time + 0.1) * sound.rate));
        if (until > pushed) {
            out.push(sound.left_right.data() + pushed * 2, until - pushed, sound.rate);
            pushed = until;
        }
        std::this_thread::sleep_until(due);
        plm_frame_to_rgba(frame, rgba.data(), static_cast<int>(width * 4));
        dreamcast::video::show(width, height, rgba.data());
        if (skip_pressed(held)) break;
    }
    plm_destroy(plm);
}

}  // namespace msr
