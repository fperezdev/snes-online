#pragma once

// Forward declarations/stubs so this repo can compile without bundling GGPO.
// When SNESONLINE_ENABLE_GGPO=1, your build should include the real ggpo headers.

#include <cstdint>

#if defined(SNESONLINE_ENABLE_GGPO) && SNESONLINE_ENABLE_GGPO
extern "C" {
#include <ggponet.h>
}

namespace snesonline {
using ::GGPOErrorCode;
using ::GGPOEvent;
using ::GGPOPlayer;
using ::GGPOPlayerHandle;
using ::GGPOSession;
using ::GGPOSessionCallbacks;
} // namespace snesonline

#else

namespace snesonline {

struct GGPOSession;
struct GGPOEvent;

typedef int GGPOErrorCode;

struct GGPOSessionCallbacks {
    bool (*begin_game)(const char* game);
    bool (*save_game_state)(unsigned char** buffer, int* len, int* checksum, int frame);
    bool (*load_game_state)(unsigned char* buffer, int len);
    bool (*log_game_state)(char* filename, unsigned char* buffer, int len);
    void (*free_buffer)(void* buffer);
    bool (*advance_frame)(int flags);
    bool (*on_event)(GGPOEvent* info);
};

} // namespace snesonline

#endif
