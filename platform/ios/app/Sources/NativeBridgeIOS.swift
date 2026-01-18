import Foundation

// Swift wrapper for the Objective-C++ bridge.

final class NativeBridgeIOS {
    static func initialize(corePath: String,
                           romPath: String,
                           enableNetplay: Bool,
                           remoteHost: String,
                           remotePort: Int,
                           localPort: Int,
                           localPlayerNum: Int) -> Bool {
        return corePath.withCString { coreC in
            return romPath.withCString { romC in
                return remoteHost.withCString { hostC in
                    snesonline_ios_initialize(coreC,
                                             romC,
                                             enableNetplay,
                                             hostC,
                                             Int32(remotePort),
                                             Int32(localPort),
                                             Int32(localPlayerNum))
                }
            }
        }
    }

    static func shutdown() {
        snesonline_ios_shutdown()
    }

    static func startLoop() {
        snesonline_ios_start_loop()
    }

    static func stopLoop() {
        snesonline_ios_stop_loop()
    }

    static func setLocalInputMask(_ mask: UInt16) {
        snesonline_ios_set_local_input_mask(mask)
    }

    static func netplayStatus() -> Int {
        return Int(snesonline_ios_get_netplay_status())
    }

    static func videoWidth() -> Int { Int(snesonline_ios_get_video_width()) }
    static func videoHeight() -> Int { Int(snesonline_ios_get_video_height()) }

    static func videoBufferRGBA() -> UnsafePointer<UInt32>? {
        return snesonline_ios_get_video_buffer_rgba()
    }

    static func audioSampleRateHz() -> Int {
        return Int(snesonline_ios_get_audio_sample_rate_hz())
    }

    static func popAudio(int16Stereo dst: UnsafeMutablePointer<Int16>, framesWanted: Int) -> Int {
        return Int(snesonline_ios_pop_audio(dst, Int32(framesWanted)))
    }
}
