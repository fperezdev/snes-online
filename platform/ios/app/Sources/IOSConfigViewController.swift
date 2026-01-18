import UIKit
import UniformTypeIdentifiers

final class IOSConfigViewController: UIViewController, UIDocumentPickerDelegate {
    private enum Keys {
        static let romBookmark = "romBookmark"
        static let enableNetplay = "enableNetplay"
        static let remoteHost = "remoteHost"
        static let remotePort = "remotePort"
        static let localPort = "localPort"
        static let localPlayerNum = "localPlayerNum"
        static let showOnscreenControls = "showOnscreenControls"
    }

    // Default core location inside the app bundle.
    private let defaultBundledCoreRelPath = "cores/snes9x_libretro_ios.dylib"

    private let scroll = UIScrollView()
    private let stack = UIStackView()

    private let romLabel = UILabel()
    private let pickRomButton = UIButton(type: .system)

    private let showTouchSwitch = UISwitch()

    private let netplaySwitch = UISwitch()
    private let remoteHostField = UITextField()
    private let remotePortField = UITextField()
    private let localPortField = UITextField()
    private let localPlayerField = UITextField()

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

        startButton.setTitle("Start Game", for: .normal)
        startButton.titleLabel?.font = UIFont.boldSystemFont(ofSize: 18)
        startButton.addTarget(self, action: #selector(startGame), for: .touchUpInside)
        stack.addArrangedSubview(startButton)

        statusLabel.numberOfLines = 0
        statusLabel.textColor = .secondaryLabel
        stack.addArrangedSubview(statusLabel)

        loadPrefs()
        updateRomLabel()
        setStatus("Note: iOS needs a bundled iOS libretro core at \(defaultBundledCoreRelPath)")
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

        remoteHostField.text = d.string(forKey: Keys.remoteHost) ?? ""
        remotePortField.text = String(d.integer(forKey: Keys.remotePort) == 0 ? 7000 : d.integer(forKey: Keys.remotePort))
        localPortField.text = String(d.integer(forKey: Keys.localPort) == 0 ? 7000 : d.integer(forKey: Keys.localPort))
        localPlayerField.text = String(d.integer(forKey: Keys.localPlayerNum) == 0 ? 1 : d.integer(forKey: Keys.localPlayerNum))

        if let data = d.data(forKey: Keys.romBookmark) {
            var stale = false
            if let url = try? URL(resolvingBookmarkData: data,
                                  options: [.withSecurityScope],
                                  bookmarkDataIsStale: &stale) {
                romURL = url
            }
        }
    }

    private func savePrefs() {
        let d = UserDefaults.standard
        d.set(netplaySwitch.isOn, forKey: Keys.enableNetplay)
        d.set(showTouchSwitch.isOn, forKey: Keys.showOnscreenControls)
        d.set(remoteHostField.text ?? "", forKey: Keys.remoteHost)
        d.set(parseInt(remotePortField.text, fallback: 7000), forKey: Keys.remotePort)
        d.set(parseInt(localPortField.text, fallback: 7000), forKey: Keys.localPort)
        d.set(parseInt(localPlayerField.text, fallback: 1), forKey: Keys.localPlayerNum)

        if let url = romURL {
            if let bm = try? url.bookmarkData(options: [.withSecurityScope],
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

    private func parseInt(_ s: String?, fallback: Int) -> Int {
        guard let s = s, let v = Int(s.trimmingCharacters(in: .whitespacesAndNewlines)) else { return fallback }
        return v
    }

    @objc private func pickRom() {
        let types: [UTType] = [.data]
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: types, asCopy: false)
        picker.delegate = self
        picker.allowsMultipleSelection = false
        present(picker, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        guard let url = urls.first else { return }
        romURL = url
        updateRomLabel()
        setStatus("Selected ROM: \(url.lastPathComponent)")
    }

    @objc private func startGame() {
        guard let romURL else {
            setStatus("Pick a ROM")
            return
        }

        savePrefs()

        // Resolve core path from bundle.
        guard let coreURL = Bundle.main.url(forResource: "snes9x_libretro_ios", withExtension: "dylib", subdirectory: "cores") else {
            setStatus("Missing core: bundle does not contain \(defaultBundledCoreRelPath)")
            return
        }

        let cfg = IOSGameConfig(
            coreURL: coreURL,
            romURL: romURL,
            enableNetplay: netplaySwitch.isOn,
            remoteHost: remoteHostField.text ?? "",
            remotePort: parseInt(remotePortField.text, fallback: 7000),
            localPort: parseInt(localPortField.text, fallback: 7000),
            localPlayerNum: parseInt(localPlayerField.text, fallback: 1),
            showOnscreenControls: showTouchSwitch.isOn
        )

        let vc = IOSGameViewController(config: cfg)
        navigationController?.pushViewController(vc, animated: true)
    }
}

struct IOSGameConfig {
    let coreURL: URL
    let romURL: URL
    let enableNetplay: Bool
    let remoteHost: String
    let remotePort: Int
    let localPort: Int
    let localPlayerNum: Int
    let showOnscreenControls: Bool
}
