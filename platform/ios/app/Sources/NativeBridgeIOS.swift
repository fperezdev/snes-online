import Foundation

// Swift wrapper for the Objective-C++ bridge.

final class NativeBridgeIOS {
    static func initialize(corePath: String,
                           romPath: String,
                           statePath: String,
                           savePath: String,
                           enableNetplay: Bool,
                           remoteHost: String,
                           remotePort: Int,
                           localPort: Int,
                           localPlayerNum: Int,
                           roomServerUrl: String,
                           roomCode: String) -> Bool {
        return corePath.withCString { coreC in
            return romPath.withCString { romC in
                return statePath.withCString { stateC in
                    return savePath.withCString { saveC in
                        return remoteHost.withCString { hostC in
                            return roomServerUrl.withCString { roomUrlC in
                                return roomCode.withCString { roomCodeC in
                                    snesonline_ios_initialize(coreC,
                                                             romC,
                                                             stateC,
                                                             saveC,
                                                             enableNetplay,
                                                             hostC,
                                                             Int32(remotePort),
                                                             Int32(localPort),
                                                             Int32(localPlayerNum),
                                                             roomUrlC,
                                                             roomCodeC)
                                }
                            }
                        }
                    }
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

    static func setPaused(_ paused: Bool) {
        snesonline_ios_set_paused(paused)
    }

    static func saveState(toPath path: String) -> Bool {
        return path.withCString { p in
            snesonline_ios_save_state_to_file(p)
        }
    }

    static func loadState(fromPath path: String) -> Bool {
        return path.withCString { p in
            snesonline_ios_load_state_from_file(p)
        }
    }

    static func stunPublicUdpPort(localPort: Int) -> Int {
        return Int(snesonline_ios_stun_public_udp_port(Int32(localPort)))
    }

    static func stunMappedAddress(localPort: Int) -> String {
        guard let cstr = snesonline_ios_stun_mapped_address(Int32(localPort)) else { return "" }
        return String(cString: cstr)
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
