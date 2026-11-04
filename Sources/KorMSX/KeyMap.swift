import Foundation

/// A key in the MSX keyboard matrix (MAME msx_keyboard.cpp KEY0-KEY10).
struct MatrixKey: Hashable {
    let row: Int
    let bit: Int

    static let shift = MatrixKey(row: 6, bit: 0)
    static let ctrl = MatrixKey(row: 6, bit: 1)
    static let graph = MatrixKey(row: 6, bit: 2)
    static let caps = MatrixKey(row: 6, bit: 3)
    static let code = MatrixKey(row: 6, bit: 4)      // CODE / KANA / 한글
    static let enter = MatrixKey(row: 7, bit: 7)
    static let space = MatrixKey(row: 8, bit: 0)
}

/// Mac key codes to MSX keys. Letters, digits and the special keys are mapped
/// by position; other printable characters can be typed by their symbol
/// ("character mode"), pressing or releasing SHIFT as the MSX layout needs.
enum KeyMap {
    /// Keys always mapped by position (Mac virtual key code -> matrix key).
    static let positional: [UInt16: MatrixKey] = {
        var m: [UInt16: MatrixKey] = [:]
        func k(_ code: UInt16, _ row: Int, _ bit: Int) { m[code] = MatrixKey(row: row, bit: bit) }
        // row 0-1: digits and the symbol keys right of them
        for (i, code) in [29, 18, 19, 20, 21, 23, 22, 26].enumerated() { k(UInt16(code), 0, i) }
        k(28, 1, 0); k(25, 1, 1)
        k(27, 1, 2); k(24, 1, 3); k(42, 1, 4); k(33, 1, 5); k(30, 1, 6); k(41, 1, 7)
        k(39, 2, 0); k(50, 2, 1); k(43, 2, 2); k(47, 2, 3); k(44, 2, 4)
        k(10, 2, 5); k(94, 2, 5)          // ISO § key / JIS _ key: the dead key or _
        k(93, 1, 4)                       // JIS ¥ key: ₩ on a Korean MSX
        // letters: A is row 2 bit 6, then in alphabetical order
        let letters: [UInt16] = [0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6]
        for (i, code) in letters.enumerated() { k(code, 2 + (i + 6) / 8, (i + 6) % 8) }
        // row 6: modifiers and F1-F3
        k(56, 6, 0); k(60, 6, 0)          // Shift
        k(59, 6, 1); k(62, 6, 1)          // Control
        k(58, 6, 2)                       // left Option = GRAPH
        k(61, 6, 4)                       // right Option = CODE / 한글
        k(102, 6, 4); k(104, 6, 4)        // 영문/한글 (JIS Eisu/Kana) keys
        k(122, 6, 5); k(120, 6, 6); k(99, 6, 7)
        // row 7
        k(118, 7, 0); k(96, 7, 1)         // F4, F5
        k(53, 7, 2); k(48, 7, 3)          // Esc, Tab
        k(100, 7, 4)                      // F8 = STOP
        k(51, 7, 5)                       // Delete (backspace) = BS
        k(101, 7, 6)                      // F9 = SELECT
        k(36, 7, 7); k(76, 7, 7)          // Return, keypad Enter
        // row 8
        k(49, 8, 0)                       // Space
        k(115, 8, 1)                      // Home = HOME/CLS
        k(114, 8, 2); k(109, 8, 2)        // Help/Insert, F10 = INS
        k(117, 8, 3)                      // forward Delete = DEL
        k(123, 8, 4); k(126, 8, 5); k(125, 8, 6); k(124, 8, 7)
        return m
    }()

    static let letterCodes: Set<UInt16> = [0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6]
    static let modifierCodes: Set<UInt16> = [56, 60, 59, 62, 58, 61, 57]

    /// Symbol keys: the character on each key, unshifted and shifted.
    private static let intlSymbols: [(MatrixKey, Character?, Character?)] = symbolRows([
        (0, ["0)", "1!", "2@", "3#", "4$", "5%", "6^", "7&"]),
        (1, ["8*", "9(", "-_", "=+", "\\|", "[{", "]}", ";:"]),
        (2, ["'\"", "`~", ",<", ".>", "/?"]),
    ])

    private static let koreanSymbols: [(MatrixKey, Character?, Character?)] = symbolRows([
        (0, ["0 ", "1!", "2\"", "3#", "4$", "5%", "6&", "7'"]),
        (1, ["8(", "9)", "-=", "^~", "\\|", "@`", "[{", ";+"]),
        (2, [":*", "]}", ",<", ".>", "/?", " _"]),
    ])

    private static func symbolRows(_ rows: [(Int, [String])]) -> [(MatrixKey, Character?, Character?)] {
        rows.flatMap { row, keys in
            keys.enumerated().map { bit, pair -> (MatrixKey, Character?, Character?) in
                let c = Array(pair)
                return (MatrixKey(row: row, bit: bit), c[0] == " " ? nil : c[0], c[1] == " " ? nil : c[1])
            }
        }
    }

    /// The MSX key and SHIFT state that type `ch`, or nil.
    static func combo(for ch: Character, layout: KeyboardLayout) -> (key: MatrixKey, shift: Bool)? {
        if let ascii = ch.asciiValue {
            switch ascii {
            case 0x61...0x7a: return (letterKey(Int(ascii - 0x61)), false)
            case 0x41...0x5a: return (letterKey(Int(ascii - 0x41)), true)
            case 0x20: return (.space, false)
            case 0x0a, 0x0d: return (.enter, false)
            default: break
            }
        }
        for (key, plain, shifted) in (layout == .korean ? koreanSymbols : intlSymbols) {
            if plain == ch { return (key, false) }
            if shifted == ch { return (key, true) }
        }
        if layout == .korean && ch == "₩" { return (MatrixKey(row: 1, bit: 4), false) }
        return nil
    }

    private static func letterKey(_ i: Int) -> MatrixKey {
        MatrixKey(row: 2 + (i + 6) / 8, bit: (i + 6) % 8)
    }

    /// US layout characters of the symbol keys, for when an input method (e.g. Korean) is active.
    static let usSymbols: [UInt16: (Character, Character)] = [
        18: ("1", "!"), 19: ("2", "@"), 20: ("3", "#"), 21: ("4", "$"), 23: ("5", "%"), 22: ("6", "^"),
        26: ("7", "&"), 28: ("8", "*"), 25: ("9", "("), 29: ("0", ")"),
        27: ("-", "_"), 24: ("=", "+"), 33: ("[", "{"), 30: ("]", "}"), 42: ("\\", "|"),
        41: (";", ":"), 39: ("'", "\""), 43: (",", "<"), 47: (".", ">"), 44: ("/", "?"), 50: ("`", "~"),
    ]
}
