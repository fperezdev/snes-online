#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0=off, 1=connecting (no peer), 2=waiting (missing inputs), 3=ok
int snesonline_ios_get_netplay_status(void);

bool snesonline_ios_initialize(const char* corePath,
                              const char* romPath,
                              bool enableNetplay,
                              const char* remoteHost,
                              int remotePort,
                              int localPort,
                              int localPlayerNum);

void snesonline_ios_shutdown(void);

void snesonline_ios_start_loop(void);
void snesonline_ios_stop_loop(void);

// Input mask is SNES bits (see include/snesonline/InputBits.h)
void snesonline_ios_set_local_input_mask(uint16_t mask);

// Video (RGBA8888 stored as 0xAARRGGBB uint32, backing buffer is 512x512)
int snesonline_ios_get_video_width(void);
int snesonline_ios_get_video_height(void);
const uint32_t* snesonline_ios_get_video_buffer_rgba(void);

// Audio (stereo S16)
int snesonline_ios_get_audio_sample_rate_hz(void);
int snesonline_ios_pop_audio(int16_t* dstInterleavedStereo, int framesWanted);

#ifdef __cplusplus
} // extern "C"
#endif
