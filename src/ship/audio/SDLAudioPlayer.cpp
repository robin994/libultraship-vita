#include "ship/audio/SDLAudioPlayer.h"
#include <spdlog/spdlog.h>

/* TEMPORARY DIAGNOSTIC (compile-time, not getenv - Vita has no shell/launch
 * mechanism to set an env var before the process starts, unlike desktop
 * builds where SSB64_GAME_THREAD_CAP_RESUMES/SSB64_TRACE_SWITCH_CTX are
 * genuinely settable). Flip to 0 to restore normal audio device init.
 *
 * Tested 1 (SDLAudioP2 thread never created) on real hardware: the
 * recurring inflate_fast crash still happened, ruling out this specific
 * thread as the corruption source. Also surfaced an unrelated, unexplained
 * side effect - "adding game archive" took ~40s+ instead of ~1s with audio
 * disabled - not chased down. Left at 0 (normal behavior) since the
 * experiment's answer is already captured in this comment. */
#define SSB64_DIAG_NO_AUDIO_DEVICE 0

namespace Ship {

SDLAudioPlayer::~SDLAudioPlayer() {
    SPDLOG_TRACE("destruct SDL audio player");
    DoClose();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void SDLAudioPlayer::DoClose() {
    if (mDevice != 0) {
        // Pause playback first
        SDL_PauseAudioDevice(mDevice, 1);
        // Clear any queued audio to prevent glitches when reopening
        SDL_ClearQueuedAudio(mDevice);
        SDL_CloseAudioDevice(mDevice);
        mDevice = 0;
    }
}

bool SDLAudioPlayer::DoInit() {
#if SSB64_DIAG_NO_AUDIO_DEVICE
    /* TEMPORARY DIAGNOSTIC: testing whether SDLAudioP2 - a genuinely
     * separate real OS thread SDL spins up inside SDL_OpenAudioDevice below,
     * confirmed running concurrently with the main coroutine thread in
     * every real-hardware coredump captured this session - is a source of
     * heap corruption that then surfaces non-deterministically elsewhere
     * (window init, archive loading, malloc, zlib - all observed as crash
     * sites on identical binaries across consecutive runs). Skips opening
     * the actual audio device (and therefore its thread) entirely while
     * leaving everything else - the AudioPlayer object, DoPlay/Buffered
     * call sites - untouched, so nothing downstream needs to null-check
     * around a missing player. */
    SPDLOG_WARN("SSB64_DIAG_NO_AUDIO_DEVICE set - skipping SDL_OpenAudioDevice entirely");
    mNumChannels = this->GetNumOutputChannels();
    mDevice = 0;
    return true;
#else
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        SPDLOG_ERROR("SDL init error: {}", SDL_GetError());
        return false;
    }

    // Always open with the correct number of output channels
    mNumChannels = this->GetNumOutputChannels();

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = this->GetSampleRate();
    want.format = AUDIO_S16SYS;
    want.channels = mNumChannels;
    want.samples = this->GetSampleLength();
    want.callback = NULL;

    mDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (mDevice == 0) {
        SPDLOG_ERROR("SDL_OpenAudio error: {}", SDL_GetError());
        return false;
    }

    SPDLOG_INFO("SDL Audio initialized: {} channels, {} Hz", mNumChannels, this->GetSampleRate());

    SDL_PauseAudioDevice(mDevice, 0);
    return true;
#endif
}

int SDLAudioPlayer::Buffered() {
    return SDL_GetQueuedAudioSize(mDevice) / (sizeof(int16_t) * mNumChannels);
}

void SDLAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
    if (Buffered() < 6000) {
        // Don't fill the audio buffer too much in case this happens
        SDL_QueueAudio(mDevice, buf, len);
    }
}
} // namespace Ship
