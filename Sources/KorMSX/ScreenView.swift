import AppKit
import MSXCore

/// Displays the MSX framebuffer and turns Mac key events into MSX matrix keys.
@MainActor
final class ScreenView: NSView {
    let emulator: Emulator
    var onCartridgeDropped: ((URL) -> Void)?
    /// Type symbols by character (Shift as the MSX needs) rather than by key position.
    var characterMode = true

    private let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
    /// MSX key held for each Mac key, and whether it forced SHIFT.
    private var down: [UInt16: (key: MatrixKey, forcedShift: Bool?)] = [:]

    init(emulator: Emulator) {
        self.emulator = emulator
        let size = emulator.screenSize
        let scale = size.width > 300 ? 1.5 : 3.0
        super.init(frame: NSRect(x: 0, y: 0, width: Double(size.width) * scale, height: Double(size.height) * scale))
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.contentsGravity = .resizeAspect
        layer?.magnificationFilter = .nearest
        registerForDraggedTypes([.fileURL])
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override var acceptsFirstResponder: Bool { true }

    func refresh() {
        let (w, h) = emulator.screenSize
        let image: CGImage? = emulator.withFramebuffer { fb in
            let data = Data(bytes: fb, count: w * h * 4) as CFData
            guard let provider = CGDataProvider(data: data) else { return nil }
            return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                           space: colorSpace,
                           bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                           provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        }
        layer?.contents = image
    }

    // MARK: Keyboard

    override func keyDown(with event: NSEvent) {
        if event.modifierFlags.contains(.command) {
            super.keyDown(with: event)
            return
        }
        let code = event.keyCode
        if event.isARepeat || down[code] != nil { return }   // the MSX repeats keys itself

        var forced: Bool?
        var key = KeyMap.positional[code]
        let byPosition = KeyMap.letterCodes.contains(code) || event.modifierFlags.contains(.control) ||
            event.modifierFlags.contains(.option)
        if characterMode && !byPosition, let ch = typedCharacter(event),
           let combo = KeyMap.combo(for: ch, layout: emulator.machine.layout) {
            key = combo.key
            forced = combo.shift
        }
        guard let key else { return }
        down[code] = (key, forced)
        if let forced { emulator.setShiftOverride(forced) }
        emulator.setKey(key, down: true)
    }

    override func keyUp(with event: NSEvent) {
        guard let entry = down.removeValue(forKey: event.keyCode) else { return }
        emulator.setKey(entry.key, down: false)
        if entry.forcedShift != nil {
            // Keep any other forced key's SHIFT state, else follow the real Shift key.
            emulator.setShiftOverride(down.values.compactMap(\.forcedShift).last)
        }
    }

    /// The character a symbol key types on a US layout, also while a Korean input method is on.
    private func typedCharacter(_ event: NSEvent) -> Character? {
        if let s = event.characters, let c = s.first, s.count == 1, c.isASCII, !(c.asciiValue! < 0x20) {
            return c
        }
        guard let pair = KeyMap.usSymbols[event.keyCode] else { return nil }
        return event.modifierFlags.contains(.shift) ? pair.1 : pair.0
    }

    override func flagsChanged(with event: NSEvent) {
        let code = event.keyCode
        if code == 57 {           // Caps Lock reports a toggle, not press/release
            emulator.tap(.caps)
            return
        }
        guard let key = KeyMap.positional[code] else { return }
        let flag: NSEvent.ModifierFlags
        switch code {
        case 56, 60: flag = .shift
        case 59, 62: flag = .control
        default: flag = .option
        }
        let isDown = event.modifierFlags.contains(flag) && down[code] == nil
        if isDown {
            down[code] = (key, nil)
            emulator.setKey(key, down: true)
        } else if let entry = down.removeValue(forKey: code) {
            emulator.setKey(entry.key, down: false)
        }
    }

    override func resignFirstResponder() -> Bool {
        down.removeAll()
        emulator.releaseAll()
        return true
    }

    // MARK: Drag and drop

    private func droppedCartridge(_ sender: NSDraggingInfo) -> URL? {
        let urls = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self]) as? [URL]
        return urls?.first { Emulator.cartridgeExtensions.contains($0.pathExtension.lowercased()) }
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        droppedCartridge(sender) == nil ? [] : .copy
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        guard let url = droppedCartridge(sender) else { return false }
        onCartridgeDropped?(url)
        return true
    }
}
