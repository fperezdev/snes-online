import UIKit
import UniformTypeIdentifiers
import CryptoKit

final class IOSConfigViewController: UIViewController, UIDocumentPickerDelegate {
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

    // Default core location inside the app bundle.
    // In CI/AltStore packaging we move the dylib to Frameworks/ for better signing compatibility.
    private let defaultBundledCoreRelPath = "Frameworks/snes9x_libretro_ios.dylib"

    private let scroll = UIScrollView()
    private let stack = UIStackView()

    private let romLabel = UILabel()
    private let pickRomButton = UIButton(type: .system)

    private let showTouchSwitch = UISwitch()
    private let showSaveSwitch = UISwitch()

    private let netplaySwitch = UISwitch()
    private let remoteHostField = UITextField()
    private let remotePortField = UITextField()
    private let localPortField = UITextField()
    private let localPlayerField = UITextField()

    private let connectionCodeField = UITextView()
    private let generateCodeButton = UIButton(type: .system)
    private let applyCodeButton = UIButton(type: .system)
    private let copyCodeButton = UIButton(type: .system)

    private let statusLabel = UILabel()
    private let startButton = UIButton(type: .system)

    private var romURL: URL?

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "SnesOnline"
        view.backgroundColor = .systemBackground

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

        romLabel.numberOfLines = 0
        romLabel.text = "ROM: (none)"

        pickRomButton.setTitle("Pick ROM", for: .normal)
        pickRomButton.addTarget(self, action: #selector(pickRom), for: .touchUpInside)

        stack.addArrangedSubview(makeSectionTitle("Controls"))
        stack.addArrangedSubview(makeRow(label: "Show on-screen controls", control: showTouchSwitch))
        stack.addArrangedSubview(makeRow(label: "Show in-game State button", control: showSaveSwitch))

        stack.addArrangedSubview(makeSectionTitle("ROM"))
        stack.addArrangedSubview(romLabel)
        stack.addArrangedSubview(pickRomButton)

        stack.addArrangedSubview(makeSectionTitle("Netplay"))
        stack.addArrangedSubview(makeRow(label: "Enable netplay", control: netplaySwitch))

        remoteHostField.placeholder = "Remote host (blank=auto if Player 1 host)"
        remoteHostField.borderStyle = .roundedRect
        remotePortField.placeholder = "Remote UDP port (7000)"
        remotePortField.borderStyle = .roundedRect
        remotePortField.keyboardType = .numberPad
        localPortField.placeholder = "Local UDP port (7000)"
        localPortField.borderStyle = .roundedRect
        localPortField.keyboardType = .numberPad
        localPlayerField.placeholder = "Local player num (1 or 2)"
        localPlayerField.borderStyle = .roundedRect
        localPlayerField.keyboardType = .numberPad

        stack.addArrangedSubview(remoteHostField)
        stack.addArrangedSubview(remotePortField)
        stack.addArrangedSubview(localPortField)
        stack.addArrangedSubview(localPlayerField)

        stack.addArrangedSubview(makeSectionTitle("Connection Code (STUN)"))

        generateCodeButton.setTitle("Generate Host Code", for: .normal)
        generateCodeButton.addTarget(self, action: #selector(generateHostCode), for: .touchUpInside)
        stack.addArrangedSubview(generateCodeButton)

        connectionCodeField.layer.borderColor = UIColor.secondaryLabel.cgColor
        connectionCodeField.layer.borderWidth = 1
        connectionCodeField.layer.cornerRadius = 8
        connectionCodeField.font = UIFont.monospacedSystemFont(ofSize: 13, weight: .regular)
        connectionCodeField.text = ""
        connectionCodeField.heightAnchor.constraint(equalToConstant: 90).isActive = true
        stack.addArrangedSubview(connectionCodeField)

        let codeButtons = UIStackView()
        codeButtons.axis = .horizontal
        codeButtons.spacing = 12
        codeButtons.distribution = .fillEqually
        copyCodeButton.setTitle("Copy", for: .normal)
        copyCodeButton.addTarget(self, action: #selector(copyCode), for: .touchUpInside)
        applyCodeButton.setTitle("Apply Code (Join)", for: .normal)
        applyCodeButton.addTarget(self, action: #selector(applyConnectionCode), for: .touchUpInside)
        codeButtons.addArrangedSubview(copyCodeButton)
        codeButtons.addArrangedSubview(applyCodeButton)
        stack.addArrangedSubview(codeButtons)

        startButton.setTitle("Start Game", for: .normal)
        startButton.titleLabel?.font = UIFont.boldSystemFont(ofSize: 18)
        startButton.addTarget(self, action: #selector(startGame), for: .touchUpInside)
        stack.addArrangedSubview(startButton)

        statusLabel.numberOfLines = 0
        statusLabel.textColor = .secondaryLabel
        stack.addArrangedSubview(statusLabel)

        loadPrefs()
        updateRomLabel()
        setStatus("Note: iOS needs a bundled iOS libretro core (Resources/cores or Frameworks).")
    }

    private func makeSectionTitle(_ text: String) -> UILabel {
        let l = UILabel()
        l.text = text
        l.font = UIFont.boldSystemFont(ofSize: 18)
        return l
    }

    private func makeRow(label: String, control: UIView) -> UIView {
        let row = UIStackView()
        row.axis = .horizontal
        row.spacing = 12
        row.alignment = .center

        let l = UILabel()
        l.text = label
        l.setContentHuggingPriority(.defaultLow, for: .horizontal)

        control.setContentHuggingPriority(.required, for: .horizontal)

        row.addArrangedSubview(l)
        row.addArrangedSubview(control)
        return row
    }

    private func loadPrefs() {
        let d = UserDefaults.standard
        netplaySwitch.isOn = d.bool(forKey: Keys.enableNetplay)
        showTouchSwitch.isOn = d.object(forKey: Keys.showOnscreenControls) == nil ? true : d.bool(forKey: Keys.showOnscreenControls)
        showSaveSwitch.isOn = d.bool(forKey: Keys.showSaveButton)

        remoteHostField.text = d.string(forKey: Keys.remoteHost) ?? ""
        remotePortField.text = String(d.integer(forKey: Keys.remotePort) == 0 ? 7000 : d.integer(forKey: Keys.remotePort))
        localPortField.text = String(d.integer(forKey: Keys.localPort) == 0 ? 7000 : d.integer(forKey: Keys.localPort))
        localPlayerField.text = String(d.integer(forKey: Keys.localPlayerNum) == 0 ? 1 : d.integer(forKey: Keys.localPlayerNum))

        connectionCodeField.text = d.string(forKey: Keys.connectionCode) ?? ""

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
        d.set(remoteHostField.text ?? "", forKey: Keys.remoteHost)
        d.set(parseInt(remotePortField.text, fallback: 7000), forKey: Keys.remotePort)
        d.set(parseInt(localPortField.text, fallback: 7000), forKey: Keys.localPort)
        d.set(parseInt(localPlayerField.text, fallback: 1), forKey: Keys.localPlayerNum)
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
            romLabel.text = "ROM: \(url.lastPathComponent)"
        } else {
            romLabel.text = "ROM: (none)"
        }
    }

    private func setStatus(_ msg: String) {
        statusLabel.text = msg
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
                setStatus("Imported ROM: \(destURL.lastPathComponent)")
            } catch {
                romURL = pickedURL
                updateRomLabel()
                setStatus("Selected ROM: \(pickedURL.lastPathComponent) (import failed: \(error.localizedDescription))")
            }
        }

        if let coordError {
            // If coordination failed, still allow selecting the URL so the user can try again.
            romURL = pickedURL
            updateRomLabel()
            setStatus("Selected ROM: \(pickedURL.lastPathComponent) (import failed: \(coordError.localizedDescription))")
        }
    }

    @objc private func startGame() {
        guard let romURL else {
            setStatus("Pick a ROM")
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
            remoteHost: remoteHostField.text ?? "",
            remotePort: parseInt(remotePortField.text, fallback: 7000),
            localPort: parseInt(localPortField.text, fallback: 7000),
            localPlayerNum: parseInt(localPlayerField.text, fallback: 1),
            showOnscreenControls: showTouchSwitch.isOn,
            showSaveButton: showSaveSwitch.isOn,
            roomServerUrl: "",
            roomCode: ""
        )

        let vc = IOSGameViewController(config: cfg)
        navigationController?.pushViewController(vc, animated: true)
    }

    @objc private func generateHostCode() {
        let lp = parseInt(localPortField.text, fallback: 7000)
        setStatus("Querying STUN for mapped address...")
        DispatchQueue.global(qos: .userInitiated).async {
            let mapped = NativeBridgeIOS.stunMappedAddress(localPort: lp)
            let code = ConnectionCode.encode(publicEndpoint: mapped)
            DispatchQueue.main.async {
                if code.isEmpty {
                    self.setStatus("STUN failed (or no network). Try again or enter remote host manually.")
                } else {
                    self.connectionCodeField.text = code
                    self.setStatus("Host code generated. Share it with Player 2.")
                }
            }
        }
    }

    @objc private func applyConnectionCode() {
        do {
            let ep = try ConnectionCode.decodePublicEndpoint(from: connectionCodeField.text ?? "")
            remoteHostField.text = ep.host
            remotePortField.text = String(ep.port)
            localPlayerField.text = "2"
            netplaySwitch.isOn = true
            setStatus("Applied code. You are Player 2 (join).")
        } catch {
            setStatus("Invalid code: \(error.localizedDescription)")
        }
    }

    @objc private func copyCode() {
        let s = (connectionCodeField.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        guard !s.isEmpty else { return }
        UIPasteboard.general.string = s
        setStatus("Copied connection code")
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
