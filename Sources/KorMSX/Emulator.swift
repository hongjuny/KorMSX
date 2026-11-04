import Foundation
import MSXCore

/// Swift wrapper around the C core: builds the machine from a `Machine`
/// layout, inserts cartridges, and owns keyboard state, typing and audio.
@MainActor
final class Emulator {
    static let frameRate = Double(MSX_CPU_CLOCK) / Double(MSX_CYCLES_PER_LINE * MSX_LINES_PER_FRAME)   // 59.92 Hz

    private var m: OpaquePointer
    let audio = AudioOutput()
    private(set) var machine: Machine
    private var library: ROMLibrary
    private(set) var cartridgeURLs: [URL?] = [nil, nil]
    private var cartridgeData: [Data?] = [nil, nil]
    private(set) var cartridgeMappers: [Int32] = [-1, -1]
    private var audioScratch = [Int16](repeating: 0, count: 8192)

    // Keyboard: several host keys may hold the same MSX key.
    private var held: [MatrixKey: Int] = [:]
    private var physicalShift = false
    private var shiftOverride: Bool?
    private var taps: [(key: MatrixKey, frames: Int)] = []
    private var typeQueue: [(key: MatrixKey, shift: Bool)] = []
    private var typePhase = 0

    enum EmulatorError: LocalizedError {
        case missingROMs(Machine, [String], [URL])
        case badCartridge(String)

        var errorDescription: String? {
            switch self {
            case let .missingROMs(machine, labels, dirs):
                return "\(machine.name)의 ROM을 찾지 못했습니다.\n\n" + labels.joined(separator: "\n") +
                    "\n\n찾아본 폴더:\n" + dirs.map(\.path).joined(separator: "\n")
            case let .badCartridge(name):
                return "\(name): 카트리지 ROM 이미지로 읽을 수 없습니다."
            }
        }
    }

    init(machine: Machine, library: ROMLibrary) throws {
        self.machine = machine
        self.library = library
        m = msx_create()
        try build()
    }

    // `m` is released with the machine; the emulator lives for the app's lifetime.

    private func build() throws {
        let images: [Data]
        switch library.images(for: machine) {
        case let .success(d): images = d
        case let .failure(missing): throw EmulatorError.missingROMs(machine, missing.labels, library.folders)
        }
        let fresh = msx_create()!
        msx_set_vdp(fresh, machine.vdp)
        for slot in machine.expandedSlots { msx_expand_slot(fresh, Int32(slot)) }
        for p in machine.placements {
            let data = images[p.file]
            // A part of the file (MAME's region offset), or all of a smaller file, mirrored.
            let length = min(data.count - p.offset, p.pages * 0x4000)
            guard length > 0 else { continue }
            _ = data.withUnsafeBytes {
                msx_add_rom(fresh, Int32(p.prim), Int32(p.sec), Int32(p.page), Int32(p.pages), $0.baseAddress! + p.offset, length)
            }
        }
        switch machine.ram {
        case let .plain(prim, sec, page, pages):
            msx_add_ram(fresh, Int32(prim), Int32(sec), Int32(page), Int32(pages))
        case let .mapper(prim, sec, kb):
            msx_add_mapper_ram(fresh, Int32(prim), Int32(sec), Int32(kb))
        }
        if let k = machine.kanji {
            _ = images[k].withUnsafeBytes { msx_add_kanji_rom(fresh, $0.baseAddress, $0.count) }
        }
        for (i, prim) in machine.cartridgeSlots.prefix(2).enumerated() {
            msx_add_cartridge_slot(fresh, Int32(i), Int32(prim), 0)
        }
        for i in 0..<2 where i < machine.cartridgeSlots.count {
            if let data = cartridgeData[i] {
                cartridgeMappers[i] = data.withUnsafeBytes {
                    msx_insert_cartridge(fresh, Int32(i), $0.baseAddress, $0.count, MSX_MAPPER_AUTO)
                }
            }
        }
        msx_reset(fresh)
        msx_destroy(m)
        m = fresh
        held.removeAll()
        typeQueue.removeAll()
        taps.removeAll()
    }

    /// Switches to another machine, keeping the cartridge in slot 1 if it still fits.
    func switchMachine(to newMachine: Machine) throws {
        let old = machine
        machine = newMachine
        if newMachine.cartridgeSlots.count < 2 {
            cartridgeURLs[1] = nil
            cartridgeData[1] = nil
        }
        do {
            try build()
        } catch {
            machine = old
            throw error
        }
    }

    func setROMLibrary(_ lib: ROMLibrary) throws {
        library = lib
        try build()
    }

    /// Power off and on: RAM and all state are cleared.
    func powerCycle() { try? build() }

    /// The reset button.
    func reset() {
        msx_reset(m)
        msx_release_all_keys(m)
        held.removeAll()
    }

    // MARK: Cartridges

    nonisolated static let cartridgeExtensions = ["rom", "mx1", "mx2", "bin", "zip"]

    func insertCartridge(_ url: URL, slot: Int) throws {
        var data = try Data(contentsOf: url)
        if url.pathExtension.lowercased() == "zip" {
            // The biggest ROM-like entry of the archive.
            let entries = ROMLibrary.zipEntries(url)
                .filter { ["rom", "mx1", "mx2", "bin"].contains(($0.name as NSString).pathExtension.lowercased()) }
            guard let entry = entries.max(by: { $0.size < $1.size }),
                  let d = ROMLibrary.unzip(url, entry: entry.name) else {
                throw EmulatorError.badCartridge(url.lastPathComponent)
            }
            data = d
        }
        guard data.count >= 0x2000, data.count <= 0x400000 else { throw EmulatorError.badCartridge(url.lastPathComponent) }
        cartridgeURLs[slot] = url
        cartridgeData[slot] = data
        try build()
        if cartridgeMappers[slot] < 0 {
            cartridgeURLs[slot] = nil
            cartridgeData[slot] = nil
            throw EmulatorError.badCartridge(url.lastPathComponent)
        }
    }

    func ejectCartridge(slot: Int) {
        cartridgeURLs[slot] = nil
        cartridgeData[slot] = nil
        cartridgeMappers[slot] = -1
        try? build()
    }

    static func mapperName(_ mapper: Int32) -> String {
        switch mapper {
        case Int32(MSX_MAPPER_PLAIN.rawValue): return "일반 ROM"
        case Int32(MSX_MAPPER_KONAMI.rawValue): return "Konami"
        case Int32(MSX_MAPPER_KONAMI_SCC.rawValue): return "Konami SCC"
        case Int32(MSX_MAPPER_ASCII8.rawValue): return "ASCII 8K"
        case Int32(MSX_MAPPER_ASCII16.rawValue): return "ASCII 16K"
        default: return "?"
        }
    }

    // MARK: Running

    func runFrame() {
        updateTyping()
        msx_run_frame(m)
        for i in taps.indices.reversed() {
            taps[i].frames -= 1
            if taps[i].frames <= 0 {
                setKey(taps[i].key, down: false)
                taps.remove(at: i)
            }
        }
        let n = audioScratch.withUnsafeMutableBufferPointer { msx_read_audio(m, $0.baseAddress, $0.count) }
        audioScratch.withUnsafeBufferPointer { audio.push(UnsafeBufferPointer(rebasing: $0[0..<n])) }
    }

    func withFramebuffer<T>(_ body: (UnsafePointer<UInt32>) -> T) -> T {
        body(msx_framebuffer(m))
    }

    /// Framebuffer size: 288x224 on an MSX1, 544x480 on an MSX2 (same pixel aspect).
    var screenSize: (width: Int, height: Int) {
        (Int(msx_screen_width(m)), Int(msx_screen_height(m)))
    }

    var codeLED: Bool { msx_code_led(m) }
    var capsLED: Bool { msx_caps_led(m) }

    // MARK: Keyboard

    func setKey(_ key: MatrixKey, down: Bool) {
        if key == .shift {
            physicalShift = down
            applyShift()
            return
        }
        let n = max(0, (held[key] ?? 0) + (down ? 1 : -1))
        held[key] = n
        msx_key(m, Int32(key.row), Int32(key.bit), n > 0)
    }

    /// Forces SHIFT on or off while a character-mode key is held (nil = follow the real Shift key).
    func setShiftOverride(_ shift: Bool?) {
        shiftOverride = shift
        applyShift()
    }

    private func applyShift() {
        msx_key(m, 6, 0, shiftOverride ?? physicalShift)
    }

    /// Presses a key for a few frames (used for Caps Lock, which the Mac reports as a toggle).
    func tap(_ key: MatrixKey) {
        setKey(key, down: true)
        taps.append((key, 3))
    }

    func releaseAll() {
        held.removeAll()
        physicalShift = false
        shiftOverride = nil
        msx_release_all_keys(m)
    }

    // MARK: Paste

    func paste(_ text: String) {
        for ch in text.replacingOccurrences(of: "\r\n", with: "\n") {
            if let c = KeyMap.combo(for: ch, layout: machine.layout) { typeQueue.append(c) }
        }
    }

    var isTyping: Bool { !typeQueue.isEmpty }

    // Each character: press (with SHIFT as needed) for 2 frames, then release for 2.
    private func updateTyping() {
        guard let next = typeQueue.first else { return }
        switch typePhase {
        case 0:
            msx_key(m, 6, 0, next.shift)
            msx_key(m, Int32(next.key.row), Int32(next.key.bit), true)
        case 2:
            msx_key(m, Int32(next.key.row), Int32(next.key.bit), false)
            msx_key(m, 6, 0, physicalShift)
        default: break
        }
        typePhase += 1
        // Enter needs longer: BASIC tokenises the line before reading the next key.
        if typePhase >= (next.key == .enter ? 8 : 4) {
            typePhase = 0
            typeQueue.removeFirst()
        }
    }
}
