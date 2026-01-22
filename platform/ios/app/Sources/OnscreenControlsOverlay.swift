import UIKit

// Minimal on-screen controls overlay for iOS.
// Produces SNES input masks (see include/snesonline/InputBits.h).

final class OnscreenControlsOverlay: UIView {
    // SNES bits (must match include/snesonline/InputBits.h)
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

    private var inputMask: UInt16 = 0 {
        didSet { onMaskChanged?(inputMask) }
    }

    var onMaskChanged: ((UInt16) -> Void)?
    var onStateRequested: (() -> Void)?

    // Mirrors Android's OnscreenControlsView behavior.
    var showStateButton: Bool = false {
        didSet {
            state.isHidden = !showStateButton
            setNeedsLayout()
        }
    }

    private let dpad = UIView()
    private let a = UIButton(type: .system)
    private let b = UIButton(type: .system)
    private let x = UIButton(type: .system)
    private let y = UIButton(type: .system)
    private let start = UIButton(type: .system)
    private let select = UIButton(type: .system)
    private let state = UIButton(type: .system)
    private let l = UIButton(type: .system)
    private let r = UIButton(type: .system)

    private let dpadCrossLayer = CAShapeLayer()

    // Track touches for the dpad region.
    private var dpadTouches: [ObjectIdentifier: CGPoint] = [:]

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        backgroundColor = .clear

        // Semi-transparent in-game controls (backgrounds only).
        let controlAlpha: CGFloat = 150.0 / 255.0
        let fill = UiStyle.primaryBase.withAlphaComponent(controlAlpha)
        let stroke = UiStyle.accent2.withAlphaComponent(controlAlpha)

        dpad.backgroundColor = fill
        dpad.layer.borderColor = stroke.cgColor
        dpad.layer.borderWidth = 2
        dpad.clipsToBounds = true

        dpadCrossLayer.fillColor = UiStyle.accent2.withAlphaComponent(200.0 / 255.0).cgColor
        dpad.layer.addSublayer(dpadCrossLayer)

        [a,b,x,y,start,select,state,l,r].forEach { btn in
            btn.backgroundColor = fill
            btn.layer.borderColor = stroke.cgColor
            btn.layer.borderWidth = 2
            btn.clipsToBounds = true
            btn.setTitleColor(UiStyle.textDetail, for: .normal)
            btn.titleLabel?.font = UIFont.systemFont(ofSize: 28, weight: .semibold)
            btn.addTarget(self, action: #selector(btnDown(_:)), for: .touchDown)
            btn.addTarget(self, action: #selector(btnUp(_:)), for: [.touchUpInside, .touchUpOutside, .touchCancel])
        }

        // Make ABXY a bit larger, like Android.
        [a,b,x,y].forEach { btn in
            btn.titleLabel?.font = UIFont.systemFont(ofSize: 28 * 1.2, weight: .bold)
        }

        // STATE is not a SNES bit; it's a UI action.
        state.addTarget(self, action: #selector(stateTapped), for: .touchUpInside)

        a.setTitle("A", for: .normal)
        b.setTitle("B", for: .normal)
        x.setTitle("X", for: .normal)
        y.setTitle("Y", for: .normal)
        start.setTitle("START", for: .normal)
        select.setTitle("SELECT", for: .normal)
        state.setTitle("STATE", for: .normal)
        l.setTitle("L", for: .normal)
        r.setTitle("R", for: .normal)

        addSubview(dpad)
        [a,b,x,y,start,select,state,l,r].forEach(addSubview)

        state.isHidden = true
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let w = bounds.width
        let h = bounds.height

        // Match Android sizing rules from OnscreenControlsView.onSizeChanged.
        let pad: CGFloat = max(24, min(w, h) * 0.04)
        let btnBase: CGFloat = max(72, min(w, h) * 0.13)
        let btn: CGFloat = (btnBase * 1.8) * 1.2 * 0.9
        let small: CGFloat = btnBase * 0.62
        let topH: CGFloat = max(54, min(w, h) * 0.08)

        // L/R along the top (reduce width by ~30%).
        let lrFullW = (w * 0.35) - pad
        let lrW = lrFullW * 0.7
        l.frame = CGRect(x: pad, y: pad, width: lrW, height: topH)
        r.frame = CGRect(x: w - pad - lrW, y: pad, width: lrW, height: topH)

        // D-pad bottom-left.
        let dSize: CGFloat = (btnBase * 1.35) * 1.8 * 1.1
        dpad.frame = CGRect(x: pad, y: h - pad - dSize, width: dSize, height: dSize)
        dpad.layer.cornerRadius = dSize * 0.5

        // D-pad cross (SVG-like) inside the circle.
        let svgInset: CGFloat = max(6, min(dpad.bounds.width, dpad.bounds.height) * 0.08)
        let crossBounds = dpad.bounds.insetBy(dx: svgInset, dy: svgInset)
        dpadCrossLayer.frame = dpad.bounds
        dpadCrossLayer.path = makeDpadCrossPath(in: crossBounds).cgPath

        // Buttons bottom-right in a diamond.
        let diamondOffset: CGFloat = 0.58
        let diamondExtent: CGFloat = diamondOffset + 0.60
        let bx = w - pad - btn * diamondExtent
        let by = h - pad - btn * diamondExtent

        a.frame = CGRect(x: bx + btn * diamondOffset, y: by, width: btn * 0.60, height: btn * 0.60)
        b.frame = CGRect(x: bx, y: by + btn * diamondOffset, width: btn * 0.60, height: btn * 0.60)
        x.frame = CGRect(x: bx, y: by - btn * diamondOffset, width: btn * 0.60, height: btn * 0.60)
        y.frame = CGRect(x: bx - btn * diamondOffset, y: by, width: btn * 0.60, height: btn * 0.60)

        // Start/Select/State center bottom.
        let midY = h - pad - small
        let midW = max(120, w * 0.16)
        let gap = max(10, pad * 0.45)
        let count: CGFloat = showStateButton ? 3 : 2
        let total = midW * count + gap * (count - 1)
        let left = (w - total) * 0.5

        select.frame = CGRect(x: left, y: midY, width: midW, height: small)
        if showStateButton {
            state.frame = CGRect(x: left + midW + gap, y: midY, width: midW, height: small)
            start.frame = CGRect(x: left + (midW + gap) * 2, y: midY, width: midW, height: small)
        } else {
            start.frame = CGRect(x: left + midW + gap, y: midY, width: midW, height: small)
        }

        // Corner radii match Android: round=oval, rect=18dp, pill=24dp-ish.
        [a, b, x, y].forEach { $0.layer.cornerRadius = $0.bounds.width * 0.5 }
        [select, start, state].forEach { $0.layer.cornerRadius = 18 }
        [l, r].forEach { $0.layer.cornerRadius = $0.bounds.height * 0.5 }
    }

    private func makeDpadCrossPath(in rect: CGRect) -> UIBezierPath {
        let path = UIBezierPath()

        let cx = rect.midX
        let cy = rect.midY
        let size = min(rect.width, rect.height)
        let arm = size * 0.18
        let th = size * 0.13

        // Vertical bar
        path.append(UIBezierPath(rect: CGRect(x: cx - th * 0.5, y: cy - arm, width: th, height: arm * 2)))
        // Horizontal bar
        path.append(UIBezierPath(rect: CGRect(x: cx - arm, y: cy - th * 0.5, width: arm * 2, height: th)))

        return path
    }

    @objc private func stateTapped() {
        onStateRequested?()
    }

    @objc private func btnDown(_ sender: UIButton) {
        let bit = bitForButton(sender)
        inputMask |= bit
    }

    @objc private func btnUp(_ sender: UIButton) {
        let bit = bitForButton(sender)
        inputMask &= ~bit
    }

    private func bitForButton(_ button: UIButton) -> UInt16 {
        switch button {
        case a: return SNES.A
        case self.b: return SNES.B
        case x: return SNES.X
        case y: return SNES.Y
        case l: return SNES.L
        case r: return SNES.R
        case start: return SNES.Start
        case select: return SNES.Select
        default: return 0
        }
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesBegan(touches, with: event)
        updateDpadTouches(touches, remove: false)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesMoved(touches, with: event)
        updateDpadTouches(touches, remove: false)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesEnded(touches, with: event)
        updateDpadTouches(touches, remove: true)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        super.touchesCancelled(touches, with: event)
        updateDpadTouches(touches, remove: true)
    }

    private func updateDpadTouches(_ touches: Set<UITouch>, remove: Bool) {
        for t in touches {
            let id = ObjectIdentifier(t)
            if remove {
                dpadTouches.removeValue(forKey: id)
            } else {
                dpadTouches[id] = t.location(in: self)
            }
        }
        recomputeDpadMask()
    }

    private func recomputeDpadMask() {
        var any = false
        var dirMask: UInt16 = 0

        for (_, p) in dpadTouches {
            if dpad.frame.contains(p) {
                any = true
                let cx = dpad.frame.midX
                let cy = dpad.frame.midY
                let rx = (p.x - cx) / (dpad.frame.width * 0.5)
                let ry = (p.y - cy) / (dpad.frame.height * 0.5)
                let dead: CGFloat = 0.18

                if rx < -dead { dirMask |= SNES.Left }
                if rx > dead { dirMask |= SNES.Right }
                if ry < -dead { dirMask |= SNES.Up }
                if ry > dead { dirMask |= SNES.Down }
            }
        }

        inputMask &= ~(SNES.Up | SNES.Down | SNES.Left | SNES.Right)
        if any { inputMask |= dirMask }
    }
}
