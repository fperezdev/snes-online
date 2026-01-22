import UIKit
import AVFoundation
import GameController

final class IOSGameViewController: UIViewController {
    // Must match include/snesonline/InputBits.h
    private enum SNES {
        static let B: UInt16 = 1 << 0
        static let Y: UInt16 = 1 << 1
        static let Select: UInt16 = 1 << 2
        static let Start: UInt16 = 1 << 3
        static let Up: UInt16 = 1 << 4
        static let Down: UInt16 = 1 << 5
        static let Left: UInt16 = 1 << 6
        static let Right: UInt16 = 1 << 7
        static let A: UInt16 = 1 << 8
        static let X: UInt16 = 1 << 9
        static let L: UInt16 = 1 << 10
        static let R: UInt16 = 1 << 11
    }

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

    private var noVideoTicks: Int = 0

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

        if config.showSaveButton {
            navigationItem.rightBarButtonItem = UIBarButtonItem(title: "State",
                                                                style: .plain,
                                                                target: self,
                                                                action: #selector(showStateMenu))
        }

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
            statePath: config.stateURL.path,
            savePath: config.saveURL.path,
            enableNetplay: config.enableNetplay,
            remoteHost: config.remoteHost,
            remotePort: config.remotePort,
            localPort: config.localPort,
            localPlayerNum: config.localPlayerNum,
            roomServerUrl: config.roomServerUrl,
            roomCode: config.roomCode
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
        // Best-effort: make sure audio is allowed to play.
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
            try session.setActive(true)
        } catch {
            // Best-effort.
        }

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
                        out.update(from: src.baseAddress!, count: gotShorts)
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

            if lx < -dead { m |= SNES.Left }
            if lx > dead { m |= SNES.Right }
            if ly < -dead { m |= SNES.Up }
            if ly > dead { m |= SNES.Down }

            // Typical mapping: A->B, B->A, X->Y, Y->X
            if pad.buttonA.isPressed { m |= SNES.B }
            if pad.buttonB.isPressed { m |= SNES.A }
            if pad.buttonX.isPressed { m |= SNES.Y }
            if pad.buttonY.isPressed { m |= SNES.X }

            if pad.leftShoulder.isPressed { m |= SNES.L }
            if pad.rightShoulder.isPressed { m |= SNES.R }

            if pad.buttonMenu.isPressed { m |= SNES.Start }
            if #available(iOS 14.0, *), pad.buttonOptions?.isPressed == true {
                m |= SNES.Select
            }
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
            else if st == 4 { waitingLabel.text = "SYNCING STATE..." }
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
        guard w > 0, h > 0, let ptr = NativeBridgeIOS.videoBufferRGBA() else {
            noVideoTicks += 1
            if noVideoTicks >= 60 {
                waitingLabel.isHidden = false
                waitingLabel.text = "No video frames yet.\n(core running?)"
            }
            return
        }
        noVideoTicks = 0

        // Build CGImage from 512x512 backing buffer but crop to w/h.
        let maxW = 512
        let bytesPerRow = maxW * 4
        let dataSize = bytesPerRow * 512

        let data = Data(bytesNoCopy: UnsafeMutableRawPointer(mutating: ptr), count: dataSize, deallocator: .none)
        guard let provider = CGDataProvider(data: data as CFData) else { return }

        let cs = CGColorSpaceCreateDeviceRGB()
        // Native buffer is 32-bit XRGB on little-endian (bytes: B,G,R,X).
        // Use noneSkipFirst so a 0 alpha byte doesn't make the image fully transparent.
        let bitmapInfo: CGBitmapInfo = [
            .byteOrder32Little,
            CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue)
        ]

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

    @objc private func showStateMenu() {
        NativeBridgeIOS.setPaused(true)

        let alert = UIAlertController(title: "State", message: nil, preferredStyle: .actionSheet)
        alert.addAction(UIAlertAction(title: "Save", style: .default) { [weak self] _ in
            guard let self else { return }
            let ok = NativeBridgeIOS.saveState(toPath: self.config.stateURL.path)
            self.NativeBridgeIOS_setPausedFalseLater()
            self.showToast(ok ? "Saved" : "Save failed")
        })
        alert.addAction(UIAlertAction(title: "Load", style: .default) { [weak self] _ in
            guard let self else { return }
            let ok = NativeBridgeIOS.loadState(fromPath: self.config.stateURL.path)
            self.NativeBridgeIOS_setPausedFalseLater()
            self.showToast(ok ? "Loaded" : "Load failed")
        })
        alert.addAction(UIAlertAction(title: "Cancel", style: .cancel) { [weak self] _ in
            self?.NativeBridgeIOS_setPausedFalseLater()
        })

        // iPad: anchor to bar button.
        if let pop = alert.popoverPresentationController {
            pop.barButtonItem = navigationItem.rightBarButtonItem
        }
        present(alert, animated: true)
    }

    private func NativeBridgeIOS_setPausedFalseLater() {
        // Allow one runloop tick for any sync-related netplay work.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            NativeBridgeIOS.setPaused(false)
        }
    }

    private func showToast(_ msg: String) {
        let l = UILabel()
        l.text = msg
        l.textColor = .white
        l.backgroundColor = UIColor.black.withAlphaComponent(0.7)
        l.textAlignment = .center
        l.layer.cornerRadius = 10
        l.layer.masksToBounds = true
        l.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(l)
        NSLayoutConstraint.activate([
            l.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            l.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor, constant: -20),
            l.widthAnchor.constraint(greaterThanOrEqualToConstant: 120),
            l.heightAnchor.constraint(equalToConstant: 44)
        ])
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
            l.removeFromSuperview()
        }
    }
}
