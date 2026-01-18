import UIKit
import AVFoundation
import GameController

final class IOSGameViewController: UIViewController {
    private let config: IOSGameConfig

    private let imageView = UIImageView()
    private let waitingLabel = UILabel()

    private var displayLink: CADisplayLink?

    private var audioEngine: AVAudioEngine?
    private var sourceNode: AVAudioSourceNode?
    private var audioTmp: [Int16] = Array(repeating: 0, count: 1024 * 2)

    private var baseMask: UInt16 = 0
    private var overlayMask: UInt16 = 0

    private var overlay: OnscreenControlsOverlay?

    init(config: IOSGameConfig) {
        self.config = config
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .black

        imageView.contentMode = .scaleAspectFit
        imageView.translatesAutoresizingMaskIntoConstraints = false

        waitingLabel.translatesAutoresizingMaskIntoConstraints = false
        waitingLabel.numberOfLines = 0
        waitingLabel.textAlignment = .center
        waitingLabel.textColor = .white
        waitingLabel.font = UIFont.boldSystemFont(ofSize: 20)
        waitingLabel.text = "Starting..."

        view.addSubview(imageView)
        view.addSubview(waitingLabel)

        NSLayoutConstraint.activate([
            imageView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            imageView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            imageView.topAnchor.constraint(equalTo: view.topAnchor),
            imageView.bottomAnchor.constraint(equalTo: view.bottomAnchor),

            waitingLabel.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            waitingLabel.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            waitingLabel.leadingAnchor.constraint(greaterThanOrEqualTo: view.leadingAnchor, constant: 24),
            waitingLabel.trailingAnchor.constraint(lessThanOrEqualTo: view.trailingAnchor, constant: -24)
        ])

        if config.showOnscreenControls {
            let ov = OnscreenControlsOverlay(frame: .zero)
            ov.translatesAutoresizingMaskIntoConstraints = false
            ov.onMaskChanged = { [weak self] mask in
                self?.overlayMask = mask
                self?.pushInput()
            }
            view.addSubview(ov)
            NSLayoutConstraint.activate([
                ov.leadingAnchor.constraint(equalTo: view.leadingAnchor),
                ov.trailingAnchor.constraint(equalTo: view.trailingAnchor),
                ov.topAnchor.constraint(equalTo: view.topAnchor),
                ov.bottomAnchor.constraint(equalTo: view.bottomAnchor)
            ])
            overlay = ov
        }

        setupControllers()
        startNative()
    }

    private func startNative() {
        // Security-scoped access for ROM.
        let okAccess = config.romURL.startAccessingSecurityScopedResource()
        defer {
            if okAccess { config.romURL.stopAccessingSecurityScopedResource() }
        }

        let ok = NativeBridgeIOS.initialize(
            corePath: config.coreURL.path,
            romPath: config.romURL.path,
            enableNetplay: config.enableNetplay,
            remoteHost: config.remoteHost,
            remotePort: config.remotePort,
            localPort: config.localPort,
            localPlayerNum: config.localPlayerNum
        )

        if !ok {
            waitingLabel.text = "Failed to start.\nCheck core/ROM/netplay settings."
            return
        }

        NativeBridgeIOS.startLoop()
        startAudio()
        startRender()
    }

    private func startRender() {
        displayLink = CADisplayLink(target: self, selector: #selector(renderTick))
        displayLink?.preferredFramesPerSecond = 60
        displayLink?.add(to: .main, forMode: .common)
    }

    private func startAudio() {
        let sampleRate = max(8000, min(192000, NativeBridgeIOS.audioSampleRateHz()))
        let engine = AVAudioEngine()

        let fmt = AVAudioFormat(commonFormat: .pcmFormatInt16,
                                sampleRate: Double(sampleRate),
                                channels: 2,
                                interleaved: true)!

        let node = AVAudioSourceNode(format: fmt) { [weak self] _, _, frameCount, audioBufferList -> OSStatus in
            guard let self else { return noErr }
            let framesWanted = Int(frameCount)
            let shortsWanted = framesWanted * 2

            if self.audioTmp.count < shortsWanted {
                self.audioTmp = Array(repeating: 0, count: shortsWanted)
            }

            let gotFrames = self.audioTmp.withUnsafeMutableBufferPointer { buf in
                NativeBridgeIOS.popAudio(int16Stereo: buf.baseAddress!, framesWanted: framesWanted)
            }

            // Fill AudioBufferList
            let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
            for buffer in abl {
                guard let mData = buffer.mData else { continue }
                let out = mData.bindMemory(to: Int16.self, capacity: shortsWanted)

                if gotFrames > 0 {
                    let gotShorts = gotFrames * 2
                    self.audioTmp.withUnsafeBufferPointer { src in
                        out.assign(from: src.baseAddress!, count: gotShorts)
                    }
                    // Zero remainder
                    if gotShorts < shortsWanted {
                        out.advanced(by: gotShorts).initialize(repeating: 0, count: shortsWanted - gotShorts)
                    }
                } else {
                    out.initialize(repeating: 0, count: shortsWanted)
                }
            }
            return noErr
        }

        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: fmt)

        do {
            try engine.start()
            audioEngine = engine
            sourceNode = node
        } catch {
            // Audio is best-effort.
        }
    }

    private func setupControllers() {
        NotificationCenter.default.addObserver(self,
                                               selector: #selector(controllerChanged),
                                               name: .GCControllerDidConnect,
                                               object: nil)
        NotificationCenter.default.addObserver(self,
                                               selector: #selector(controllerChanged),
                                               name: .GCControllerDidDisconnect,
                                               object: nil)
        controllerChanged()
    }

    @objc private func controllerChanged() {
        // Nothing to do; we poll controller state on renderTick.
    }

    private func pushInput() {
        NativeBridgeIOS.setLocalInputMask(baseMask | overlayMask)
    }

    private func updateControllerMask() {
        var m: UInt16 = 0

        if let c = GCController.controllers().first(where: { $0.extendedGamepad != nil }),
           let pad = c.extendedGamepad {
            let lx = pad.leftThumbstick.xAxis.value
            let ly = pad.leftThumbstick.yAxis.value
            let dead: Float = 0.25

            if lx < -dead { m |= (1 << 10) }
            if lx > dead { m |= (1 << 11) }
            if ly < -dead { m |= (1 << 9) }
            if ly > dead { m |= (1 << 8) }

            if pad.buttonA.isPressed { m |= (1 << 1) } // map A->B
            if pad.buttonB.isPressed { m |= (1 << 0) }
            if pad.buttonX.isPressed { m |= (1 << 3) }
            if pad.buttonY.isPressed { m |= (1 << 2) }

            if pad.leftShoulder.isPressed { m |= (1 << 4) }
            if pad.rightShoulder.isPressed { m |= (1 << 5) }

            if pad.buttonMenu.isPressed { m |= (1 << 6) }
        }

        baseMask = m
    }

    @objc private func renderTick() {
        // Update netplay waiting UI.
        if config.enableNetplay {
            let st = NativeBridgeIOS.netplayStatus()
            waitingLabel.isHidden = (st == 3)
            if st == 1 { waitingLabel.text = "WAITING FOR PEER..." }
            else if st == 2 { waitingLabel.text = "WAITING FOR INPUTS..." }
            else if st == 3 { /* hidden */ }
        } else {
            waitingLabel.isHidden = true
        }

        // Controller polling -> input.
        updateControllerMask()
        pushInput()

        // Render.
        let w = NativeBridgeIOS.videoWidth()
        let h = NativeBridgeIOS.videoHeight()
        guard w > 0, h > 0, let ptr = NativeBridgeIOS.videoBufferRGBA() else { return }

        // Build CGImage from 512x512 backing buffer but crop to w/h.
        let maxW = 512
        let bytesPerRow = maxW * 4
        let dataSize = bytesPerRow * 512

        let data = Data(bytesNoCopy: UnsafeMutableRawPointer(mutating: ptr), count: dataSize, deallocator: .none)
        guard let provider = CGDataProvider(data: data as CFData) else { return }

        let cs = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue)

        guard let full = CGImage(width: maxW,
                                 height: 512,
                                 bitsPerComponent: 8,
                                 bitsPerPixel: 32,
                                 bytesPerRow: bytesPerRow,
                                 space: cs,
                                 bitmapInfo: bitmapInfo,
                                 provider: provider,
                                 decode: nil,
                                 shouldInterpolate: false,
                                 intent: .defaultIntent) else { return }

        guard let cropped = full.cropping(to: CGRect(x: 0, y: 0, width: min(w, maxW), height: min(h, 512))) else { return }
        imageView.image = UIImage(cgImage: cropped)
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        displayLink?.invalidate()
        displayLink = nil

        NativeBridgeIOS.stopLoop()
        NativeBridgeIOS.shutdown()

        audioEngine?.stop()
        audioEngine = nil
    }
}
