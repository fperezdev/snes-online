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

    private let dpad = UIView()
    private let a = UIButton(type: .system)
    private let b = UIButton(type: .system)
    private let x = UIButton(type: .system)
    private let y = UIButton(type: .system)
    private let start = UIButton(type: .system)
    private let select = UIButton(type: .system)
    private let l = UIButton(type: .system)
    private let r = UIButton(type: .system)

    // Track touches for the dpad region.
    private var dpadTouches: [ObjectIdentifier: CGPoint] = [:]

    override init(frame: CGRect) {
        super.init(frame: frame)
        isMultipleTouchEnabled = true
        backgroundColor = .clear

        dpad.backgroundColor = UIColor.black.withAlphaComponent(0.25)
        dpad.layer.cornerRadius = 16

        [a,b,x,y,start,select,l,r].forEach { btn in
            btn.backgroundColor = UIColor.black.withAlphaComponent(0.25)
            btn.layer.cornerRadius = 16
            btn.setTitleColor(.white, for: .normal)
            btn.titleLabel?.font = UIFont.boldSystemFont(ofSize: 16)
            btn.addTarget(self, action: #selector(btnDown(_:)), for: .touchDown)
            btn.addTarget(self, action: #selector(btnUp(_:)), for: [.touchUpInside, .touchUpOutside, .touchCancel])
        }

        a.setTitle("A", for: .normal)
        b.setTitle("B", for: .normal)
        x.setTitle("X", for: .normal)
        y.setTitle("Y", for: .normal)
        start.setTitle("START", for: .normal)
        select.setTitle("SELECT", for: .normal)
        l.setTitle("L", for: .normal)
        r.setTitle("R", for: .normal)

        addSubview(dpad)
        [a,b,x,y,start,select,l,r].forEach(addSubview)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let w = bounds.width
        let h = bounds.height
        let pad: CGFloat = max(16, min(w,h) * 0.03)
        let btn: CGFloat = max(72, min(w,h) * 0.13)
        let small: CGFloat = btn * 0.62
        let topH: CGFloat = max(54, min(w,h) * 0.08)

        l.frame = CGRect(x: pad, y: pad, width: w * 0.35 - pad, height: topH)
        r.frame = CGRect(x: w * 0.65, y: pad, width: w * 0.35 - pad, height: topH)

        let dSize = btn * 1.35
        dpad.frame = CGRect(x: pad, y: h - pad - dSize, width: dSize, height: dSize)

        let cx = w - pad - btn * 0.9
        let cy = h - pad - btn * 0.9
        a.frame = CGRect(x: cx + btn * 0.55, y: cy, width: btn * 0.6, height: btn * 0.6)
        b.frame = CGRect(x: cx, y: cy + btn * 0.55, width: btn * 0.6, height: btn * 0.6)
        x.frame = CGRect(x: cx, y: cy - btn * 0.55, width: btn * 0.6, height: btn * 0.6)
        y.frame = CGRect(x: cx - btn * 0.55, y: cy, width: btn * 0.6, height: btn * 0.6)

        select.frame = CGRect(x: w * 0.5 - (small * 2) - pad, y: h - pad - small, width: small * 2, height: small)
        start.frame = CGRect(x: w * 0.5 + pad, y: h - pad - small, width: small * 2, height: small)
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
