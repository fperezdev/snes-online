#import "SNESOnlineIOSBridge.h"

#import <GameController/GameController.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "snesonline/EmulatorEngine.h"
#include "snesonline/InputBits.h"
#include "snesonline/InputMapping.h"

namespace snesonline {

struct IOSControllerPollerImpl {
    std::atomic<bool> running{false};
    std::thread thread;

    // Store controller pointer without retaining; notifications manage lifecycle.
    std::atomic<void*> controllerPtr{nullptr};

    id connectObserver = nil;
    id disconnectObserver = nil;

    void updateControllerOnce() {
        // One-time scan to find an already-connected controller.
        for (GCController* controller in [GCController controllers]) {
            if (controller.extendedGamepad) {
                controllerPtr.store((__bridge void*)controller, std::memory_order_relaxed);
                break;
            }
        }
    }

    void registerObservers() {
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];

        connectObserver = [nc addObserverForName:GCControllerDidConnectNotification
                                         object:nil
                                          queue:nil
                                     usingBlock:^(NSNotification* note) {
            GCController* controller = (GCController*)note.object;
            if (controller && controller.extendedGamepad) {
                controllerPtr.store((__bridge void*)controller, std::memory_order_relaxed);
            }
        }];

        disconnectObserver = [nc addObserverForName:GCControllerDidDisconnectNotification
                                            object:nil
                                             queue:nil
                                        usingBlock:^(NSNotification* note) {
            GCController* controller = (GCController*)note.object;
            void* cur = controllerPtr.load(std::memory_order_relaxed);
            if (cur == (__bridge void*)controller) {
                controllerPtr.store(nullptr, std::memory_order_relaxed);
            }
        }];
    }

    void unregisterObservers() {
        NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
        if (connectObserver) {
            [nc removeObserver:connectObserver];
            connectObserver = nil;
        }
        if (disconnectObserver) {
            [nc removeObserver:disconnectObserver];
            disconnectObserver = nil;
        }
    }

    void run() {
        using clock = std::chrono::steady_clock;
        constexpr auto frame = std::chrono::microseconds(16667);
        auto next = clock::now();

        while (running.load(std::memory_order_relaxed)) {
            next += frame;

            uint16_t mask = 0;

            GCController* controller = (__bridge GCController*)controllerPtr.load(std::memory_order_relaxed);
            if (controller) {
                GCExtendedGamepad* pad = controller.extendedGamepad;
                if (pad) {

                    // D-pad from left thumbstick (or dpad if you prefer).
                    const float lx = pad.leftThumbstick.xAxis.value;
                    const float ly = pad.leftThumbstick.yAxis.value;
                    mask |= sanitizeDpad(mapIOSThumbstickToDpad({lx, ly}));

                    if (pad.buttonA.isPressed) mask |= SNES_B; // Common SNES layout: A->B
                    if (pad.buttonB.isPressed) mask |= SNES_A;
                    if (pad.buttonX.isPressed) mask |= SNES_Y;
                    if (pad.buttonY.isPressed) mask |= SNES_X;

                    if (pad.leftShoulder.isPressed) mask |= SNES_L;
                    if (pad.rightShoulder.isPressed) mask |= SNES_R;

                    if (pad.buttonMenu.isPressed) mask |= SNES_START;
                    // SELECT not consistently available; map options/share if present.
                }
            }

            EmulatorEngine::instance().setLocalInputMask(mask);

            std::this_thread::sleep_until(next);
        }
    }
};

IOSControllerPoller::IOSControllerPoller() noexcept {
    auto* impl = new IOSControllerPollerImpl();
    impl->updateControllerOnce();
    impl->registerObservers();
    impl_ = impl;
}

IOSControllerPoller::~IOSControllerPoller() noexcept {
    stop();
    auto* impl = static_cast<IOSControllerPollerImpl*>(impl_);
    if (impl) {
        impl->unregisterObservers();
        delete impl;
    }
    impl_ = nullptr;
}

void IOSControllerPoller::start() noexcept {
    auto* impl = static_cast<IOSControllerPollerImpl*>(impl_);
    if (!impl) return;
    if (impl->running.exchange(true, std::memory_order_relaxed)) return;

    impl->thread = std::thread([impl]() { impl->run(); });
}

void IOSControllerPoller::stop() noexcept {
    auto* impl = static_cast<IOSControllerPollerImpl*>(impl_);
    if (!impl) return;
    impl->running.store(false, std::memory_order_relaxed);
    if (impl->thread.joinable()) impl->thread.join();
}

} // namespace snesonline
