import UIKit

// Mirrors Android's `com.snesonline.UiStyle`.
// Keep these values in sync so iOS and Android feel identical.
enum UiStyle {
    // Classic SNES palette
    // Primary Base, Console Light Grey, #CEC9CC
    // Secondary Base, Console Dark Grey, #908A99
    // Accent 1, Light Lavender (Buttons), #B5B6E4
    // Accent 2, Dark Purple (Buttons), #4F43AE
    // Text/Detail, Deep Charcoal, #211A21
    static let primaryBase = UIColor(hexRGB: 0xCEC9CC)
    static let secondaryBase = UIColor(hexRGB: 0x908A99)
    static let accent1 = UIColor(hexRGB: 0xB5B6E4)
    static let accent2 = UIColor(hexRGB: 0x4F43AE)
    static let textDetail = UIColor(hexRGB: 0x211A21)

    static var configBackground: UIColor { .white }
    static var configText: UIColor { textDetail }

    // In-game dialogs
    static var stateDialogBackground: UIColor { UIColor(hexRGBA: 0xF2211A21) }
    static var saveHeader: UIColor { accent1 }
    static var loadHeader: UIColor { accent2 }
    static var saveListBackground: UIColor { UIColor(hexRGBA: 0xE6908A99) }
    static var loadListBackground: UIColor { UIColor(hexRGBA: 0xE6908A99) }
    static var dividerColor: UIColor { accent2 }

    // Interactive theming (mirrors ConfigActivity.java)
    static var enabledButtonBg: UIColor { accent2 }
    static var disabledButtonBg: UIColor { UIColor(hexRGB: 0xE6E3E5) }
    static var enabledButtonText: UIColor { .white }
    static var disabledButtonText: UIColor { primaryBase }

    static var switchChecked: UIColor { accent2 }
    static var switchUnchecked: UIColor { secondaryBase }
    static var switchTrackChecked: UIColor { accent2.withAlphaComponent(0.40) }
    static var switchTrackUnchecked: UIColor { secondaryBase.withAlphaComponent(0.33) }
}

extension UIColor {
    convenience init(hexRGB: UInt32) {
        let r = CGFloat((hexRGB >> 16) & 0xFF) / 255.0
        let g = CGFloat((hexRGB >> 8) & 0xFF) / 255.0
        let b = CGFloat(hexRGB & 0xFF) / 255.0
        self.init(red: r, green: g, blue: b, alpha: 1.0)
    }

    convenience init(hexRGBA: UInt32) {
        let a = CGFloat((hexRGBA >> 24) & 0xFF) / 255.0
        let r = CGFloat((hexRGBA >> 16) & 0xFF) / 255.0
        let g = CGFloat((hexRGBA >> 8) & 0xFF) / 255.0
        let b = CGFloat(hexRGBA & 0xFF) / 255.0
        self.init(red: r, green: g, blue: b, alpha: a)
    }
}
