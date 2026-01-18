#pragma once

#include <cstdint>

namespace snesonline {

// Start/stop background polling of GCController and feed EmulatorEngine.
// Implemented in Objective-C++ (.mm).
class IOSControllerPoller {
public:
    IOSControllerPoller() noexcept;
    ~IOSControllerPoller() noexcept;

    IOSControllerPoller(const IOSControllerPoller&) = delete;
    IOSControllerPoller& operator=(const IOSControllerPoller&) = delete;

    void start() noexcept;
    void stop() noexcept;

private:
    void* impl_ = nullptr;
};

} // namespace snesonline
