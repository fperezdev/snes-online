#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Netplay status:
// 0=off, 1=connecting (no peer yet), 2=waiting (peer but missing inputs), 3=ok, 4=syncing state
int snesonline_ios_get_netplay_status(void);

bool snesonline_ios_initialize(const char* corePath,
                              const char* romPath,
                              const char* statePath,
                              const char* savePath,
                              bool enableNetplay,
                              const char* remoteHost,
                              int remotePort,
                              int localPort,
                              int localPlayerNum,
                              const char* roomServerUrl,
                              const char* roomCode,
                              const char* sharedSecret);

void snesonline_ios_shutdown(void);

void snesonline_ios_start_loop(void);
void snesonline_ios_stop_loop(void);

// Pause emulation (loop keeps running to allow netplay state sync).
void snesonline_ios_set_paused(bool paused);

// Save states (host-only in netplay).
bool snesonline_ios_save_state_to_file(const char* statePath);
bool snesonline_ios_load_state_from_file(const char* statePath);

// Networking helpers (STUN)
int snesonline_ios_stun_public_udp_port(int localPort);
// Returns a best-effort mapped address as "ip:port" ("" on failure).
// Pointer remains valid until the next call on this thread.
const char* snesonline_ios_stun_mapped_address(int localPort);

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
