#ifdef AAPI_SDL2

#include <SDL.h>
#include <stdio.h>
#include <stdint.h>

#include "audio_api.h"

static SDL_AudioDeviceID dev;
static SDL_AudioSpec s_have;
static FILE *s_recFile;
static uint32_t s_recBytes;

static void wav_u32(FILE *f, uint32_t v) {
    fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f);
    fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f);
}
static void wav_u16(FILE *f, uint16_t v) {
    fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f);
}

/* Trailer capture: tap the queued PCM and write a WAV inside the app files
 * dir.  Toggled by `cmd audiorec --ei v 1|0`; pulled with run-as. */
void audio_sdl_record(int on) {
    if (on) {
        if (s_recFile != NULL) {
            return;
        }
        s_recFile = fopen("/data/user/0/com.vdavid003.sm64port/files/audio_rec.wav", "wb");
        if (s_recFile == NULL) {
            return;
        }
        s_recBytes = 0;
        fwrite("RIFF", 1, 4, s_recFile); wav_u32(s_recFile, 0);
        fwrite("WAVE", 1, 4, s_recFile);
        fwrite("fmt ", 1, 4, s_recFile); wav_u32(s_recFile, 16);
        wav_u16(s_recFile, 1);
        wav_u16(s_recFile, (uint16_t) s_have.channels);
        wav_u32(s_recFile, (uint32_t) s_have.freq);
        wav_u32(s_recFile, (uint32_t) (s_have.freq * s_have.channels * 2));
        wav_u16(s_recFile, (uint16_t) (s_have.channels * 2));
        wav_u16(s_recFile, 16);
        fwrite("data", 1, 4, s_recFile); wav_u32(s_recFile, 0);
    } else if (s_recFile != NULL) {
        fseek(s_recFile, 4, SEEK_SET); wav_u32(s_recFile, 36 + s_recBytes);
        fseek(s_recFile, 40, SEEK_SET); wav_u32(s_recFile, s_recBytes);
        fclose(s_recFile);
        s_recFile = NULL;
    }
}

static bool audio_sdl_init(void) {
#ifdef __ANDROID__
    // SDL's AAudio backend can crash inside aaudio_DetectBrokenPlayState ->
    // AudioTrack::getTimestamp when the audio route changes. OpenSL ES has been
    // stable on Android for years, so prefer it.
    SDL_setenv("SDL_AUDIODRIVER", "opensles", 1);
#endif
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL init error: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 32000;
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = 512;
    want.callback = NULL;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        fprintf(stderr, "SDL_OpenAudio error: %s\n", SDL_GetError());
        return false;
    }
    s_have = have;
    SDL_PauseAudioDevice(dev, 0);
    return true;
}

static int audio_sdl_buffered(void) {
    return SDL_GetQueuedAudioSize(dev) / 4;
}

static int audio_sdl_get_desired_buffered(void) {
    return 1100;
}

static void audio_sdl_play(const uint8_t *buf, size_t len) {
    if (s_recFile != NULL && len > 0) {
        fwrite(buf, 1, len, s_recFile);
        s_recBytes += (uint32_t) len;
    }
    if (audio_sdl_buffered() < 6000) {
        // Don't fill the audio buffer too much in case this happens
        SDL_QueueAudio(dev, buf, len);
    }
}

static void audio_sdl_shutdown(void) 
{
    if (SDL_WasInit(SDL_INIT_AUDIO)) {
        if (dev != 0) {
            SDL_CloseAudioDevice(dev);
            dev = 0;
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

struct AudioAPI audio_sdl = {
    audio_sdl_init,
    audio_sdl_buffered,
    audio_sdl_get_desired_buffered,
    audio_sdl_play,
    audio_sdl_shutdown
};

#endif
