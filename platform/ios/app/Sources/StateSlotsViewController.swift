import UIKit

final class StateSlotsViewController: UIViewController, UITableViewDataSource, UITableViewDelegate {
    enum Mode {
        case save
        case load
    }

    struct Slot {
        let index1: Int
        let url: URL
    }

    var slots: [Slot] = []
    var isNetplayEnabled: Bool = false

    var onClose: (() -> Void)?
    var onToast: ((String) -> Void)?
    var onNetplayWaitForReadyThenUnpause: (() -> Void)?

    private let root = UIView()
    private let saveTitle = UILabel()
    private let loadTitle = UILabel()
    private let saveTable = UITableView(frame: .zero, style: .plain)
    private let loadTable = UITableView(frame: .zero, style: .plain)
    private let divider = UIView()
    private let closeButton = UIButton(type: .system)

    override func viewDidLoad() {
        super.viewDidLoad()

        view.backgroundColor = UIColor.black.withAlphaComponent(0.0)

        root.translatesAutoresizingMaskIntoConstraints = false
        root.backgroundColor = UiStyle.stateDialogBackground
        root.layer.cornerRadius = 14
        root.clipsToBounds = true
        view.addSubview(root)

        NSLayoutConstraint.activate([
            root.centerXAnchor.constraint(equalTo: view.centerXAnchor),
            root.centerYAnchor.constraint(equalTo: view.centerYAnchor),
            root.widthAnchor.constraint(lessThanOrEqualTo: view.widthAnchor, constant: -24),
            root.widthAnchor.constraint(greaterThanOrEqualToConstant: 280),
        ])

        let content = UIStackView()
        content.axis = .vertical
        content.spacing = 12
        content.translatesAutoresizingMaskIntoConstraints = false
        content.layoutMargins = UIEdgeInsets(top: 16, left: 16, bottom: 16, right: 16)
        content.isLayoutMarginsRelativeArrangement = true
        root.addSubview(content)

        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            content.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            content.topAnchor.constraint(equalTo: root.topAnchor),
            content.bottomAnchor.constraint(equalTo: root.bottomAnchor),
        ])

        let title = UILabel()
        title.text = "STATE"
        title.textAlignment = .center
        title.textColor = .white
        title.font = UIFont.systemFont(ofSize: 18, weight: .bold)
        content.addArrangedSubview(title)

        let columns = UIStackView()
        columns.axis = .horizontal
        columns.spacing = 10
        columns.alignment = .fill
        columns.distribution = .fillEqually
        content.addArrangedSubview(columns)

        let left = UIStackView()
        left.axis = .vertical
        left.spacing = 8
        let right = UIStackView()
        right.axis = .vertical
        right.spacing = 8

        saveTitle.text = "SAVE"
        saveTitle.textAlignment = .center
        saveTitle.textColor = UiStyle.saveHeader
        saveTitle.font = UIFont.systemFont(ofSize: 18, weight: .bold)

        loadTitle.text = "LOAD"
        loadTitle.textAlignment = .center
        loadTitle.textColor = UiStyle.loadHeader
        loadTitle.font = UIFont.systemFont(ofSize: 18, weight: .bold)

        saveTable.dataSource = self
        saveTable.delegate = self
        loadTable.dataSource = self
        loadTable.delegate = self

        [saveTable, loadTable].forEach { t in
            t.separatorStyle = .singleLine
            t.separatorColor = UIColor.white.withAlphaComponent(0.55)
            t.rowHeight = 44
            t.layer.cornerRadius = 6
            t.clipsToBounds = true
            t.tableFooterView = UIView()
        }
        saveTable.backgroundColor = UiStyle.saveListBackground
        loadTable.backgroundColor = UiStyle.loadListBackground

        left.addArrangedSubview(saveTitle)
        left.addArrangedSubview(saveTable)
        right.addArrangedSubview(loadTitle)
        right.addArrangedSubview(loadTable)

        // Fixed-ish height like Android (260dp). We use ~260pt.
        saveTable.heightAnchor.constraint(equalToConstant: 260).isActive = true
        loadTable.heightAnchor.constraint(equalToConstant: 260).isActive = true

        columns.addArrangedSubview(left)

        divider.backgroundColor = UiStyle.dividerColor
        divider.translatesAutoresizingMaskIntoConstraints = false
        divider.widthAnchor.constraint(equalToConstant: 2).isActive = true
        columns.addArrangedSubview(divider)

        columns.addArrangedSubview(right)

        closeButton.setTitle("Close", for: .normal)
        closeButton.setTitleColor(.white, for: .normal)
        closeButton.backgroundColor = UiStyle.accent2
        closeButton.layer.cornerRadius = 10
        closeButton.contentEdgeInsets = UIEdgeInsets(top: 10, left: 16, bottom: 10, right: 16)
        closeButton.addTarget(self, action: #selector(closeTapped), for: .touchUpInside)
        content.addArrangedSubview(closeButton)
    }

    @objc private func closeTapped() {
        dismiss(animated: true) { [weak self] in
            self?.onClose?()
        }
    }

    // MARK: - UITableView

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        slots.count
    }

    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let id = "cell"
        let cell = tableView.dequeueReusableCell(withIdentifier: id) ?? UITableViewCell(style: .default, reuseIdentifier: id)
        cell.backgroundColor = .clear
        cell.textLabel?.textColor = .white
        cell.textLabel?.font = UIFont.systemFont(ofSize: 16, weight: .regular)
        cell.selectionStyle = .default

        let slot = slots[indexPath.row]
        let exists = FileManager.default.fileExists(atPath: slot.url.path)
        let label: String
        if exists, let attrs = try? FileManager.default.attributesOfItem(atPath: slot.url.path),
           let mtime = attrs[.modificationDate] as? Date {
            let df = DateFormatter()
            df.dateFormat = "yyyy-MM-dd HH:mm"
            let ts = df.string(from: mtime)
            label = "Slot \(slot.index1)  (\(ts))"
        } else {
            label = tableView === loadTable ? "Slot \(slot.index1)  (empty)" : "Slot \(slot.index1)"
        }
        cell.textLabel?.text = label

        return cell
    }

    func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        guard indexPath.row >= 0 && indexPath.row < slots.count else { return }
        let slot = slots[indexPath.row]

        if tableView === saveTable {
            let ok = NativeBridgeIOS.saveState(toPath: slot.url.path)
            onToast?(ok ? "Saved: slot \(slot.index1)" : "Save failed")
            saveTable.reloadData()
            loadTable.reloadData()
            return
        }

        // Load
        if !FileManager.default.fileExists(atPath: slot.url.path) {
            onToast?("Slot is empty")
            return
        }

        let ok = NativeBridgeIOS.loadState(fromPath: slot.url.path)
        if !ok {
            onToast?("Load failed")
            return
        }

        if isNetplayEnabled {
            onToast?("Syncing state...")
            dismiss(animated: true) { [weak self] in
                self?.onNetplayWaitForReadyThenUnpause?()
            }
        } else {
            onToast?("Loaded: slot \(slot.index1)")
            dismiss(animated: true) { [weak self] in
                self?.onClose?()
            }
        }
    }
}
