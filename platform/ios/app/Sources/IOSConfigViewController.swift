import UIKit
import UniformTypeIdentifiers
import CryptoKit

final class IOSConfigViewController: UIViewController, UIDocumentPickerDelegate, UITextFieldDelegate {
    private enum Keys {
        static let romBookmark = "romBookmark"
        static let enableNetplay = "enableNetplay"
        static let remoteHost = "remoteHost"
        static let remotePort = "remotePort"
        static let localPort = "localPort"
        static let localPlayerNum = "localPlayerNum"
        static let showOnscreenControls = "showOnscreenControls"
        static let showSaveButton = "showSaveButton"
        static let connectionCode = "connectionCode"
    }

    // Mirrors Android's ConnectionUiState.
    private enum ConnectionUiState {
        case idle
        case hostReady
        case joinInput
        case joinReady
    }

    // Default core location inside the app bundle.
    // In CI/AltStore packaging we move the dylib to Frameworks/ for better signing compatibility.
    private let defaultBundledCoreRelPath = "Frameworks/snes9x_libretro_ios.dylib"

    private let scroll = UIScrollView()
    private let stack = UIStackView()

    private let headerLabel = UILabel()

    private let romTitleLabel = UILabel()
    private let romPathField = UITextField()
    private let pickRomButton = UIButton(type: .system)

    private let showTouchSwitch = UISwitch()
    private let showSaveSwitch = UISwitch()

    private let audioTitleLabel = UILabel()

    private let netplayTitleLabel = UILabel()
    private let netplaySwitch = UISwitch()
    private let localPortField = UITextField()

    private let startConnectionButton = UIButton(type: .system)
    private let orLabel = UILabel()
    private let connectionCodeField = UITextView()
    private let joinConnectionButton = UIButton(type: .system)

    private let hostWaitingRow = UIStackView()
    private let hostWaitingLabel = UILabel()
    private let copyConnectionIconButton = UIButton(type: .system)

    private let joinTargetLabel = UILabel()
    private let cancelConnectionButton = UIButton(type: .system)

    private let startButton = UIButton(type: .system)
    private let statusLabel = UILabel()

    private var romURL: URL?

    private var connectionUiState: ConnectionUiState = .idle
    private var lastHostPublicEndpoint: String = "" // ip:port

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "SNES ONLINE"
        view.backgroundColor = UiStyle.configBackground

        stack.axis = .vertical
        stack.spacing = 12
        stack.alignment = .fill

        scroll.translatesAutoresizingMaskIntoConstraints = false
        stack.translatesAutoresizingMaskIntoConstraints = false

        view.addSubview(scroll)
        scroll.addSubview(stack)

        NSLayoutConstraint.activate([
            scroll.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor),
            scroll.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            scroll.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor),

            stack.leadingAnchor.constraint(equalTo: scroll.contentLayoutGuide.leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: scroll.contentLayoutGuide.trailingAnchor, constant: -16),
            stack.topAnchor.constraint(equalTo: scroll.contentLayoutGuide.topAnchor, constant: 16),
            stack.bottomAnchor.constraint(equalTo: scroll.contentLayoutGuide.bottomAnchor, constant: -16),
            stack.widthAnchor.constraint(equalTo: scroll.frameLayoutGuide.widthAnchor, constant: -32)
        ])

        // Header (Android has no action bar; this is our in-content title).
        headerLabel.text = "SNES ONLINE"
        headerLabel.font = UIFont.systemFont(ofSize: 22, weight: .bold)
        headerLabel.textColor = UiStyle.configText
        headerLabel.textAlignment = .left
        stack.addArrangedSubview(headerLabel)

        // ROM (.sfc/.smc)
        romTitleLabel.text = "ROM (.sfc/.smc)"
        romTitleLabel.font = UIFont.systemFont(ofSize: 16, weight: .regular)
        romTitleLabel.textColor = UiStyle.configText
        stack.addArrangedSubview(romTitleLabel)

        romPathField.borderStyle = .roundedRect
        romPathField.placeholder = "Pick a ROM file"
        romPathField.delegate = self
        romPathField.textColor = UiStyle.configText
        stack.addArrangedSubview(romPathField)

        pickRomButton.setTitle("Pick ROM", for: .normal)
        pickRomButton.addTarget(self, action: #selector(pickRom), for: .touchUpInside)
        stack.addArrangedSubview(pickRomButton)

        // Switches (touch / state)
        stack.addArrangedSubview(makeRow(label: "Show on-screen controls (touch)", control: showTouchSwitch))
        stack.addArrangedSubview(makeRow(label: "Show in-game State button", control: showSaveSwitch))

        // Audio (header only, like Android)
        audioTitleLabel.text = "Audio"
        audioTitleLabel.font = UIFont.systemFont(ofSize: 16, weight: .regular)
        audioTitleLabel.textColor = UiStyle.configText
        audioTitleLabel.layoutMargins = UIEdgeInsets(top: 12, left: 0, bottom: 0, right: 0)
        stack.addArrangedSubview(audioTitleLabel)

        // Netplay
        netplayTitleLabel.text = "Netplay"
        netplayTitleLabel.font = UIFont.systemFont(ofSize: 16, weight: .regular)
        netplayTitleLabel.textColor = UiStyle.configText
        stack.addArrangedSubview(netplayTitleLabel)

        stack.addArrangedSubview(makeRow(label: "Enable Netplay", control: netplaySwitch))

        localPortField.borderStyle = .roundedRect
        localPortField.placeholder = "Local UDP port (default 7000)"
        localPortField.keyboardType = .numberPad
        localPortField.textColor = UiStyle.configText
        stack.addArrangedSubview(localPortField)

        startConnectionButton.setTitle("Start connection", for: .normal)
        startConnectionButton.addTarget(self, action: #selector(startConnectionTapped), for: .touchUpInside)
        stack.addArrangedSubview(startConnectionButton)

        orLabel.text = "OR"
        orLabel.textAlignment = .center
        orLabel.textColor = UiStyle.configText
        stack.addArrangedSubview(orLabel)

        connectionCodeField.layer.borderColor = UiStyle.dividerColor.cgColor
        connectionCodeField.layer.borderWidth = 1
        connectionCodeField.layer.cornerRadius = 8
        connectionCodeField.font = UIFont.monospacedSystemFont(ofSize: 13, weight: .regular)
        connectionCodeField.textColor = UiStyle.configText
        connectionCodeField.backgroundColor = .white
        connectionCodeField.text = ""
        connectionCodeField.heightAnchor.constraint(equalToConstant: 90).isActive = true
        stack.addArrangedSubview(connectionCodeField)

        joinConnectionButton.setTitle("Join connection", for: .normal)
        joinConnectionButton.addTarget(self, action: #selector(joinConnectionTapped), for: .touchUpInside)
        stack.addArrangedSubview(joinConnectionButton)

        hostWaitingRow.axis = .horizontal
        hostWaitingRow.spacing = 6
        hostWaitingRow.alignment = .center
        hostWaitingLabel.numberOfLines = 0
        hostWaitingLabel.textColor = UiStyle.configText
        hostWaitingRow.addArrangedSubview(hostWaitingLabel)
        copyConnectionIconButton.setImage(UIImage(systemName: "doc.on.doc"), for: .normal)
        copyConnectionIconButton.tintColor = UiStyle.configText
        copyConnectionIconButton.widthAnchor.constraint(equalToConstant: 40).isActive = true
        copyConnectionIconButton.heightAnchor.constraint(equalToConstant: 40).isActive = true
        copyConnectionIconButton.addTarget(self, action: #selector(copyCode), for: .touchUpInside)
        hostWaitingRow.addArrangedSubview(copyConnectionIconButton)
        stack.addArrangedSubview(hostWaitingRow)

        joinTargetLabel.numberOfLines = 0
        joinTargetLabel.textColor = UiStyle.configText
        stack.addArrangedSubview(joinTargetLabel)

        cancelConnectionButton.setTitle("Cancel", for: .normal)
        cancelConnectionButton.addTarget(self, action: #selector(cancelConnectionTapped), for: .touchUpInside)
        stack.addArrangedSubview(cancelConnectionButton)

        startButton.setTitle("Start Game", for: .normal)
        startButton.titleLabel?.font = UIFont.systemFont(ofSize: 18, weight: .bold)
        startButton.addTarget(self, action: #selector(startGame), for: .touchUpInside)
        stack.addArrangedSubview(startButton)

        statusLabel.numberOfLines = 0
        statusLabel.textColor = UiStyle.configText
        stack.addArrangedSubview(statusLabel)

        // Hook change events.
        netplaySwitch.addTarget(self, action: #selector(netplayToggled), for: .valueChanged)
        showTouchSwitch.addTarget(self, action: #selector(anySettingChanged), for: .valueChanged)
        showSaveSwitch.addTarget(self, action: #selector(anySettingChanged), for: .valueChanged)
        localPortField.addTarget(self, action: #selector(anySettingChanged), for: .editingChanged)

        loadPrefs()
        applyConfigTheme()
        applyInteractiveTheme()
        updateRomLabel()
        applyConnectionUiState(.idle)
        updateStartButtonEnabled()
        setStatus("")
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        navigationController?.setNavigationBarHidden(true, animated: animated)
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        navigationController?.setNavigationBarHidden(false, animated: animated)
    }

    private func makeRow(label: String, control: UIView) -> UIView {
        let row = UIStackView()
        row.axis = .horizontal
        row.spacing = 12
        row.alignment = .center

        let l = UILabel()
        l.text = label
        l.textColor = UiStyle.configText
        l.setContentHuggingPriority(.defaultLow, for: .horizontal)

        control.setContentHuggingPriority(.required, for: .horizontal)

        row.addArrangedSubview(l)
        row.addArrangedSubview(control)
        return row
    }

    private func applyConfigTheme() {
        view.backgroundColor = UiStyle.configBackground
        stack.backgroundColor = UiStyle.configBackground

        // Hint color matches Android: text with alpha 160.
        let hint = UiStyle.configText.withAlphaComponent(160.0 / 255.0)
        romPathField.attributedPlaceholder = NSAttributedString(string: romPathField.placeholder ?? "", attributes: [.foregroundColor: hint])
        localPortField.attributedPlaceholder = NSAttributedString(string: localPortField.placeholder ?? "", attributes: [.foregroundColor: hint])
    }

    private func applyInteractiveTheme() {
        applyButtonTheme(pickRomButton)
        applyButtonTheme(startConnectionButton)
        applyButtonTheme(joinConnectionButton)
        applyButtonTheme(cancelConnectionButton)
        applyButtonTheme(startButton)

        applySwitchTheme(netplaySwitch)
        applySwitchTheme(showTouchSwitch)
        applySwitchTheme(showSaveSwitch)
    }

    private func applyButtonTheme(_ b: UIButton) {
        let enabledBg = UiStyle.enabledButtonBg
        let disabledBg = UiStyle.disabledButtonBg
        let enabledText = UiStyle.enabledButtonText
        let disabledText = UiStyle.disabledButtonText

        let en = b.isEnabled
        b.setTitleColor(en ? enabledText : disabledText, for: .normal)
        b.backgroundColor = en ? enabledBg : disabledBg
        b.layer.cornerRadius = 10
        b.contentEdgeInsets = UIEdgeInsets(top: 12, left: 16, bottom: 12, right: 16)
    }

    private func applySwitchTheme(_ s: UISwitch) {
        // Approximate Android's thumb/track tint behavior.
        s.onTintColor = UiStyle.switchTrackChecked
        s.thumbTintColor = s.isOn ? UiStyle.switchChecked : UiStyle.switchUnchecked
    }

    private func loadPrefs() {
        let d = UserDefaults.standard
        netplaySwitch.isOn = d.bool(forKey: Keys.enableNetplay)
        showTouchSwitch.isOn = d.object(forKey: Keys.showOnscreenControls) == nil ? true : d.bool(forKey: Keys.showOnscreenControls)
        showSaveSwitch.isOn = d.bool(forKey: Keys.showSaveButton)

        localPortField.text = String(d.integer(forKey: Keys.localPort) == 0 ? 7000 : d.integer(forKey: Keys.localPort))

        let savedCode = d.string(forKey: Keys.connectionCode) ?? ""
        connectionCodeField.text = savedCode

        if let data = d.data(forKey: Keys.romBookmark) {
            var stale = false
            let opts: URL.BookmarkResolutionOptions = []
            if let url = try? URL(resolvingBookmarkData: data,
                                  options: opts,
                                  bookmarkDataIsStale: &stale) {
                romURL = url
            }
        }
    }

    private func savePrefs() {
        let d = UserDefaults.standard
        d.set(netplaySwitch.isOn, forKey: Keys.enableNetplay)
        d.set(showTouchSwitch.isOn, forKey: Keys.showOnscreenControls)
        d.set(showSaveSwitch.isOn, forKey: Keys.showSaveButton)
        d.set(parseInt(localPortField.text, fallback: 7000), forKey: Keys.localPort)
        d.set(connectionCodeField.text ?? "", forKey: Keys.connectionCode)

        if let url = romURL {
            let opts: URL.BookmarkCreationOptions = []
            if let bm = try? url.bookmarkData(options: opts,
                                             includingResourceValuesForKeys: nil,
                                             relativeTo: nil) {
                d.set(bm, forKey: Keys.romBookmark)
            }
        }
    }

    private func updateRomLabel() {
        if let url = romURL {
            romPathField.text = url.lastPathComponent
        } else {
            romPathField.text = ""
        }
    }

    private func setStatus(_ msg: String) {
        statusLabel.text = msg
    }

    private func isValidPort(_ p: Int) -> Bool {
        return p >= 1 && p <= 65535
    }

    private func updateStartButtonEnabled() {
        // Core must exist in the bundle.
        let coreOk = (resolveBundledCoreURL() != nil)
        let romOk = (romURL != nil)
        let port = parseInt(localPortField.text, fallback: 7000)
        let portOk = isValidPort(port)

        var enabled = coreOk && romOk && portOk
        if netplaySwitch.isOn {
            enabled = enabled && (connectionUiState == .hostReady || connectionUiState == .joinReady)
        }

        startButton.isEnabled = enabled
        applyInteractiveTheme()
    }

    private func applyConnectionUiState(_ st: ConnectionUiState) {
        connectionUiState = st

        let showNetplay = netplaySwitch.isOn
        let effective: ConnectionUiState = showNetplay ? st : .idle

        orLabel.isHidden = (effective != .idle)

        startConnectionButton.isHidden = (effective == .joinInput || effective == .joinReady)

        joinConnectionButton.isHidden = (effective == .hostReady || effective == .joinReady)
        joinConnectionButton.setTitle((effective == .joinInput || effective == .joinReady) ? "ACCEPT" : "Join connection", for: .normal)

        connectionCodeField.isHidden = !(effective == .joinInput || effective == .joinReady)
        connectionCodeField.isEditable = (effective != .joinReady)

        let showHostWaiting = (effective == .hostReady && !lastHostPublicEndpoint.isEmpty)
        hostWaitingRow.isHidden = !showHostWaiting
        hostWaitingLabel.text = showHostWaiting ? ("The other player should connect at \(lastHostPublicEndpoint)") : ""
        copyConnectionIconButton.isHidden = !showHostWaiting

        if effective == .joinReady {
            let d = UserDefaults.standard
            let host = d.string(forKey: Keys.remoteHost) ?? ""
            let port = d.integer(forKey: Keys.remotePort)
            if !host.isEmpty && port > 0 {
                joinTargetLabel.text = "Will connect to \(host):\(port)"
                joinTargetLabel.isHidden = false
            } else {
                joinTargetLabel.isHidden = true
            }
        } else {
            joinTargetLabel.isHidden = true
        }

        cancelConnectionButton.isHidden = (effective == .idle)
        startConnectionButton.isEnabled = showNetplay
        joinConnectionButton.isEnabled = showNetplay
        applyInteractiveTheme()
        updateStartButtonEnabled()
    }

    @objc private func netplayToggled() {
        if !netplaySwitch.isOn {
            applyConnectionUiState(.idle)
        } else {
            applyConnectionUiState(.idle)
        }
        updateStartButtonEnabled()
    }

    @objc private func anySettingChanged() {
        applySwitchTheme(netplaySwitch)
        applySwitchTheme(showTouchSwitch)
        applySwitchTheme(showSaveSwitch)
        updateStartButtonEnabled()
    }

    @objc private func startConnectionTapped() {
        let lp = parseInt(localPortField.text, fallback: 7000)
        guard isValidPort(lp) else {
            setStatus("Local UDP port must be 1..65535")
            return
        }

        netplaySwitch.isOn = true
        applySwitchTheme(netplaySwitch)
        setStatus("Discovering public endpoint...")

        DispatchQueue.global(qos: .userInitiated).async {
            let mapped = NativeBridgeIOS.stunMappedAddress(localPort: lp)
            let code = ConnectionCode.encode(publicEndpoint: mapped)
            DispatchQueue.main.async {
                if code.isEmpty {
                    self.setStatus("STUN failed (or no network).")
                    self.applyConnectionUiState(.idle)
                    return
                }
                self.connectionCodeField.text = code
                self.lastHostPublicEndpoint = mapped

                // Host is Player 1.
                let d = UserDefaults.standard
                d.set(1, forKey: Keys.localPlayerNum)
                d.set("", forKey: Keys.remoteHost)
                d.set(7000, forKey: Keys.remotePort)

                self.setStatus("Paste the code on the other device and press Join connection")
                self.applyConnectionUiState(.hostReady)
            }
        }
    }

    @objc private func joinConnectionTapped() {
        switch connectionUiState {
        case .idle:
            netplaySwitch.isOn = true
            applySwitchTheme(netplaySwitch)
            setStatus("Paste the code")
            applyConnectionUiState(.joinInput)
        case .joinInput:
            do {
                let ep = try ConnectionCode.decodePublicEndpoint(from: connectionCodeField.text ?? "")
                let d = UserDefaults.standard
                d.set(ep.host, forKey: Keys.remoteHost)
                d.set(ep.port, forKey: Keys.remotePort)
                d.set(2, forKey: Keys.localPlayerNum)
                setStatus("")
                applyConnectionUiState(.joinReady)
            } catch {
                setStatus("Invalid code: \(error.localizedDescription)")
            }
        default:
            break
        }
    }

    @objc private func cancelConnectionTapped() {
        let d = UserDefaults.standard
        d.set("", forKey: Keys.connectionCode)
        d.set(1, forKey: Keys.localPlayerNum)
        d.set("", forKey: Keys.remoteHost)
        d.set(0, forKey: Keys.remotePort)
        lastHostPublicEndpoint = ""
        connectionCodeField.text = ""
        setStatus("")
        applyConnectionUiState(.idle)
        updateStartButtonEnabled()
    }

    private func resolveBundledCoreURL() -> (url: URL, relPath: String)? {
        let candidates: [(subdirectory: String?, relPath: String)] = [
            ("Resources/cores", "Resources/cores/snes9x_libretro_ios.dylib"),
            ("cores", "cores/snes9x_libretro_ios.dylib"),
            ("Frameworks", "Frameworks/snes9x_libretro_ios.dylib"),
            (nil, "snes9x_libretro_ios.dylib")
        ]

        for candidate in candidates {
            if let url = Bundle.main.url(
                forResource: "snes9x_libretro_ios",
                withExtension: "dylib",
                subdirectory: candidate.subdirectory
            ) {
                return (url, candidate.relPath)
            }

            if let base = Bundle.main.resourceURL {
                let url = base.appendingPathComponent(candidate.relPath)
                if FileManager.default.fileExists(atPath: url.path) {
                    return (url, candidate.relPath)
                }
            }
        }

        return nil
    }

    private func parseInt(_ s: String?, fallback: Int) -> Int {
        guard let s = s, let v = Int(s.trimmingCharacters(in: .whitespacesAndNewlines)) else { return fallback }
        return v
    }

    func textFieldShouldBeginEditing(_ textField: UITextField) -> Bool {
        if textField === romPathField {
            pickRom()
            return false
        }
        return true
    }

    @objc private func pickRom() {
        let types: [UTType] = [.data]
        // Use asCopy=true so iOS provides a local, sandbox-readable copy.
        // This is much more reliable for cloud providers (Google Drive, etc.).
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: types, asCopy: true)
        picker.delegate = self
        picker.allowsMultipleSelection = false
        present(picker, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        guard let pickedURL = urls.first else { return }

        // Import into our sandbox to avoid security-scoped / cloud-provider access issues at runtime.
        // Libretro cores typically expect a plain filesystem path they can fopen().
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        let romsDir = docs.appendingPathComponent("roms", isDirectory: true)
        try? FileManager.default.createDirectory(at: romsDir, withIntermediateDirectories: true)

        let fileName = pickedURL.lastPathComponent.isEmpty ? "game.sfc" : pickedURL.lastPathComponent
        let destURL = romsDir.appendingPathComponent(fileName)

        // Coordinate reading because some providers give transient URLs.
        let coordinator = NSFileCoordinator()
        var coordError: NSError?
        coordinator.coordinate(readingItemAt: pickedURL, options: [], error: &coordError) { readableURL in
            do {
                if FileManager.default.fileExists(atPath: destURL.path) {
                    try FileManager.default.removeItem(at: destURL)
                }

                // Try a direct copy first.
                if FileManager.default.fileExists(atPath: readableURL.path) {
                    try FileManager.default.copyItem(at: readableURL, to: destURL)
                } else {
                    // Fallback: read bytes and write (handles some file providers).
                    let data = try Data(contentsOf: readableURL)
                    try data.write(to: destURL, options: .atomic)
                }

                romURL = destURL
                updateRomLabel()
                setStatus("ROM copied to: \(destURL.lastPathComponent)")
                updateStartButtonEnabled()
            } catch {
                romURL = pickedURL
                updateRomLabel()
                setStatus("Selected ROM: \(pickedURL.lastPathComponent) (import failed: \(error.localizedDescription))")
                updateStartButtonEnabled()
            }
        }

        if let coordError {
            // If coordination failed, still allow selecting the URL so the user can try again.
            romURL = pickedURL
            updateRomLabel()
            setStatus("Selected ROM: \(pickedURL.lastPathComponent) (import failed: \(coordError.localizedDescription))")
            updateStartButtonEnabled()
        }
    }

    @objc private func startGame() {
        guard let romURL else {
            setStatus("Pick a ROM")
            return
        }

        updateStartButtonEnabled()
        guard startButton.isEnabled else {
            // Mirror Android's behavior: show a helpful status instead of doing nothing.
            if resolveBundledCoreURL() == nil {
                setStatus("Missing core")
            } else if !isValidPort(parseInt(localPortField.text, fallback: 7000)) {
                setStatus("Local UDP port must be 1..65535")
            } else if netplaySwitch.isOn {
                setStatus("Press Start connection first")
            }
            return
        }

        savePrefs()

        // Resolve core path from bundle.
        guard let core = resolveBundledCoreURL() else {
            setStatus(
                "Missing core: bundle does not contain \(defaultBundledCoreRelPath) (also tried cores/, Frameworks/, and bundle root)"
            )
            return
        }
        let coreURL = core.url
        setStatus("Using core: \(core.relPath)")

        let romBaseName = (romURL.deletingPathExtension().lastPathComponent.isEmpty ? "game" : romURL.deletingPathExtension().lastPathComponent)
        let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first!
        let statesDir = docs.appendingPathComponent("states", isDirectory: true)
        let savesDir = docs.appendingPathComponent("saves", isDirectory: true)
        try? FileManager.default.createDirectory(at: statesDir, withIntermediateDirectories: true)
        try? FileManager.default.createDirectory(at: savesDir, withIntermediateDirectories: true)
        let stateURL = statesDir.appendingPathComponent("\(romBaseName).state")
        let saveURL = savesDir.appendingPathComponent("\(romBaseName).srm")

        let cfg = IOSGameConfig(
            coreURL: coreURL,
            romURL: romURL,
            stateURL: stateURL,
            saveURL: saveURL,
            enableNetplay: netplaySwitch.isOn,
            remoteHost: UserDefaults.standard.string(forKey: Keys.remoteHost) ?? "",
            remotePort: UserDefaults.standard.integer(forKey: Keys.remotePort) == 0 ? 7000 : UserDefaults.standard.integer(forKey: Keys.remotePort),
            localPort: parseInt(localPortField.text, fallback: 7000),
            localPlayerNum: UserDefaults.standard.integer(forKey: Keys.localPlayerNum) == 0 ? 1 : UserDefaults.standard.integer(forKey: Keys.localPlayerNum),
            showOnscreenControls: showTouchSwitch.isOn,
            showSaveButton: showSaveSwitch.isOn,
            roomServerUrl: "",
            roomCode: ""
        )

        let vc = IOSGameViewController(config: cfg)
        navigationController?.pushViewController(vc, animated: true)
    }

    @objc private func copyCode() {
        let s = (connectionCodeField.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        guard !s.isEmpty else { return }
        UIPasteboard.general.string = s
        setStatus("Copied")
    }
}

struct IOSGameConfig {
    let coreURL: URL
    let romURL: URL
    let stateURL: URL
    let saveURL: URL
    let enableNetplay: Bool
    let remoteHost: String
    let remotePort: Int
    let localPort: Int
    let localPlayerNum: Int
    let showOnscreenControls: Bool
    let showSaveButton: Bool
    let roomServerUrl: String
    let roomCode: String
}

private enum ConnectionCode {
    struct Endpoint {
        let host: String
        let port: Int
    }

    static func encode(publicEndpoint: String) -> String {
        let trimmed = publicEndpoint.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.contains(":") else { return "" }

        let payload = "v=2&pub=\(trimmed)"
        let sig = shortSigB64Url(payload)
        let full = sig.isEmpty ? payload : (payload + "&sig=\(sig)")
        guard let data = full.data(using: .utf8) else { return "" }
        return "SNO2:" + base64UrlNoPad(data)
    }

    static func decodePublicEndpoint(from code: String) throws -> Endpoint {
        let t = code.trimmingCharacters(in: .whitespacesAndNewlines)
        guard t.hasPrefix("SNO2:") || t.hasPrefix("SNO1:") else {
            throw NSError(domain: "ConnectionCode", code: 1, userInfo: [NSLocalizedDescriptionKey: "Missing SNO prefix"])
        }
        let b64 = String(t.dropFirst(5))
        guard let data = base64UrlDecode(b64),
              let payload = String(data: data, encoding: .utf8) else {
            throw NSError(domain: "ConnectionCode", code: 2, userInfo: [NSLocalizedDescriptionKey: "Malformed base64"])
        }
        let parts = payload.split(separator: "&")
        var pub: String = ""
        for part in parts {
            let kv = part.split(separator: "=", maxSplits: 1)
            if kv.count == 2 && kv[0] == "pub" { pub = String(kv[1]) }
        }
        let hp = pub.split(separator: ":")
        guard hp.count == 2, let port = Int(hp[1]), port >= 1, port <= 65535 else {
            throw NSError(domain: "ConnectionCode", code: 3, userInfo: [NSLocalizedDescriptionKey: "Missing valid public endpoint"])
        }
        return Endpoint(host: String(hp[0]), port: port)
    }

    private static func shortSigB64Url(_ payload: String) -> String {
        guard let data = payload.data(using: .utf8) else { return "" }
        let hash = SHA256.hash(data: data)
        let bytes = Array(hash)
        let n = min(12, bytes.count)
        return base64UrlNoPad(Data(bytes[0..<n]))
    }

    private static func base64UrlNoPad(_ data: Data) -> String {
        var s = data.base64EncodedString()
        s = s.replacingOccurrences(of: "+", with: "-")
        s = s.replacingOccurrences(of: "/", with: "_")
        s = s.replacingOccurrences(of: "=", with: "")
        return s
    }

    private static func base64UrlDecode(_ s: String) -> Data? {
        var t = s.replacingOccurrences(of: "-", with: "+")
        t = t.replacingOccurrences(of: "_", with: "/")
        let mod = t.count % 4
        if mod == 2 { t += "==" }
        else if mod == 3 { t += "=" }
        else if mod != 0 { return nil }
        return Data(base64Encoded: t)
    }
}
