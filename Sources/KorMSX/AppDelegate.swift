import AppKit
import QuartzCore
import UniformTypeIdentifiers

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var window: NSWindow!
    private var emulator: Emulator!
    private var screen: ScreenView!
    private var timer: Timer?
    private var nextFrame = CACurrentMediaTime()
    private var paused = false
    private var machineMenu = NSMenu(title: "기기")
    private var lastTitle = ""

    private let defaults = UserDefaults.standard
    private var romFolder: URL? {
        get { defaults.url(forKey: "romFolder") }
        set { defaults.set(newValue, forKey: "romFolder") }
    }
    private var library: ROMLibrary { ROMLibrary(folders: ROMLibrary.defaultFolders(userFolder: romFolder)) }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Start the saved machine, else the first one whose ROMs are present
        // (Korean machines first), else the bundled C-BIOS.
        let saved = defaults.string(forKey: "machine")
        let lib = library
        var order = Machine.all.filter { !$0.bundled && $0.layout == .korean }
        order += Machine.all.filter { !$0.bundled && $0.layout != .korean }
        if let s = Machine.all.first(where: { $0.id == saved }) { order.insert(s, at: 0) }
        for machine in order where emulator == nil {
            if case .success = lib.images(for: machine) { emulator = try? Emulator(machine: machine, library: lib) }
        }
        if emulator == nil {
            emulator = try! Emulator(machine: Machine.all[0], library: lib)
            needsROMNotice = true
        }

        screen = ScreenView(emulator: emulator)
        screen.onCartridgeDropped = { [weak self] url in self?.insertCartridge(url, slot: 0) }
        window = NSWindow(contentRect: screen.frame, styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        window.contentView = screen
        window.contentAspectRatio = NSSize(width: emulator.screenSize.width, height: emulator.screenSize.height)
        window.delegate = self
        window.center()
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(screen)
        buildMenus()
        updateTitle()

        let t = Timer(timeInterval: 1.0 / 240.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
        RunLoop.main.add(t, forMode: .common)
        timer = t
        NSApp.activate(ignoringOtherApps: true)
        if needsROMNotice {
            DispatchQueue.main.async { [weak self] in self?.showROMNotice() }
        }
    }

    private var needsROMNotice = false

    /// Shown when no machine with BASIC has its ROMs, so only C-BIOS could start.
    private func showROMNotice() {
        let alert = NSAlert()
        alert.messageText = "BASIC ROM을 찾지 못해 C-BIOS로 시작했습니다"
        alert.informativeText = """
        C-BIOS는 카트리지만 실행하며 BASIC이 없습니다. MSX.ROM이나 한국 기기 ROM이 든 폴더를 \
        지정하면 BASIC으로 시작합니다.

        찾아본 폴더:
        \(library.folders.map(\.path).joined(separator: "\n"))
        """
        alert.addButton(withTitle: "ROM 폴더 선택…")
        alert.addButton(withTitle: "닫기")
        if alert.runModal() == .alertFirstButtonReturn { chooseROMFolder() }
        nextFrame = CACurrentMediaTime()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    // Frames are paced by wall time, nudged by the audio queue so sound neither
    // runs dry nor builds up latency.
    private func tick() {
        guard !paused else { return }
        let now = CACurrentMediaTime()
        if now - nextFrame > 0.25 { nextFrame = now }   // after a stall, don't try to catch up
        var ran = false
        var budget = 3
        while now >= nextFrame && budget > 0 {
            emulator.runFrame()
            let fill = Double(emulator.audio.fill)
            let target = Double(AudioOutput.targetFill)
            let adjust = max(-0.02, min(0.02, (fill - target) / target * 0.02))
            nextFrame += (1.0 / Emulator.frameRate) * (1.0 + adjust)
            budget -= 1
            ran = true
        }
        if ran {
            screen.refresh()
            updateTitle()
        }
    }

    private func updateTitle() {
        var title = "KorMSX — \(emulator.machine.name)"
        if let url = emulator.cartridgeURLs[0] {
            title += " — \(url.deletingPathExtension().lastPathComponent)"
        }
        if emulator.machine.layout == .korean && emulator.codeLED { title += "  [한글]" }
        if emulator.capsLED { title += "  [CAPS]" }
        if paused { title += "  (일시정지)" }
        if title != lastTitle {
            window.title = title
            lastTitle = title
        }
    }

    // MARK: Menus

    private func buildMenus() {
        let main = NSMenu()

        let appItem = NSMenuItem()
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "KorMSX 정보", action: #selector(about), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "KorMSX 종료", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        main.addItem(appItem)

        let fileItem = NSMenuItem()
        let file = NSMenu(title: "파일")
        file.addItem(withTitle: "카트리지 열기 (슬롯 1)…", action: #selector(openCartridge1), keyEquivalent: "o")
        let open2 = file.addItem(withTitle: "카트리지 열기 (슬롯 2)…", action: #selector(openCartridge2), keyEquivalent: "O")
        open2.keyEquivalentModifierMask = [.command, .shift]
        file.addItem(withTitle: "카트리지 빼기 (슬롯 1)", action: #selector(eject1), keyEquivalent: "e")
        file.addItem(withTitle: "카트리지 빼기 (슬롯 2)", action: #selector(eject2), keyEquivalent: "")
        file.addItem(.separator())
        file.addItem(withTitle: "ROM 폴더 선택…", action: #selector(chooseROMFolder), keyEquivalent: "")
        file.addItem(withTitle: "ROM 폴더 열기", action: #selector(revealROMFolder), keyEquivalent: "")
        fileItem.submenu = file
        main.addItem(fileItem)

        let editItem = NSMenuItem()
        let edit = NSMenu(title: "편집")
        edit.addItem(withTitle: "붙여넣기 (키 입력)", action: #selector(paste(_:)), keyEquivalent: "v")
        editItem.submenu = edit
        main.addItem(editItem)

        let machineItem = NSMenuItem()
        machineMenu.removeAllItems()
        machineMenu.addItem(withTitle: "리셋", action: #selector(resetMachine), keyEquivalent: "r")
        let power = machineMenu.addItem(withTitle: "전원 껐다 켜기", action: #selector(powerCycle), keyEquivalent: "R")
        power.keyEquivalentModifierMask = [.command, .shift]
        machineMenu.addItem(withTitle: "일시정지", action: #selector(togglePause), keyEquivalent: "p")
        machineMenu.addItem(.separator())
        var generation = ""
        for (i, machine) in Machine.all.enumerated() {
            if machine.generation.prefix(4) != generation.prefix(4) {
                if !generation.isEmpty { machineMenu.addItem(.separator()) }
                machineMenu.addItem(withTitle: String(machine.generation.prefix(4)), action: nil, keyEquivalent: "").isEnabled = false
                generation = machine.generation
            }
            let item = machineMenu.addItem(withTitle: machine.name, action: #selector(selectMachine(_:)), keyEquivalent: "")
            item.tag = i
            item.indentationLevel = 1
            if let missing = machine.missing { item.toolTip = "아직 에뮬레이트하지 않는 장치: \(missing)" }
        }
        machineMenu.addItem(.separator())
        machineMenu.addItem(withTitle: "기호는 문자 그대로 입력", action: #selector(toggleCharacterMode), keyEquivalent: "")
        machineMenu.addItem(withTitle: "소리 끄기", action: #selector(toggleMute), keyEquivalent: "m")
        machineMenu.addItem(.separator())
        machineMenu.addItem(withTitle: "키 배치 안내", action: #selector(showKeyHelp), keyEquivalent: "/")
        machineMenu.delegate = self
        machineItem.submenu = machineMenu
        main.addItem(machineItem)

        NSApp.mainMenu = main
    }

    @objc private func about() {
        let alert = NSAlert()
        alert.messageText = "KorMSX"
        alert.informativeText = """
        한국형 MSX1 에뮬레이터. Z80, TMS9918A, AY-3-8910, 8255 PPI, 슬롯/서브슬롯, \
        Konami/ASCII 메퍼와 SCC를 에뮬레이트합니다. 기기 구성은 MAME의 msx1.cpp 정의를 따랐습니다.

        큐닉스 한글 BASIC은 한국형 MSX1 기기의 한글 ROM으로 동작합니다(ROM은 직접 준비해야 합니다).
        내장 C-BIOS © C-BIOS 팀 (BSD 라이선스).
        """
        alert.runModal()
    }

    @objc private func showKeyHelp() {
        let alert = NSAlert()
        alert.messageText = "키 배치"
        alert.informativeText = """
        한글 (CODE): 오른쪽 Option
        GRAPH: 왼쪽 Option
        CAPS: Caps Lock
        F1–F5: F1–F5 (Shift와 함께 F6–F10)
        STOP: F8    SELECT: F9    INS: F10 또는 Help
        HOME: Home    DEL: 앞쪽 Delete    BS: Delete

        한글 BASIC에서 오른쪽 Option을 누르면 한글 LED(제목 표시줄의 [한글])가 켜지고 \
        두벌식 자판 위치로 한글을 입력합니다. 맥의 입력기는 영문으로 두는 것이 좋습니다.
        ⌘V로 붙여넣은 텍스트는 키 입력으로 전달됩니다.
        """
        alert.runModal()
    }

    // MARK: Actions

    @objc private func openCartridge1() { openCartridge(slot: 0) }
    @objc private func openCartridge2() { openCartridge(slot: 1) }
    @objc private func eject1() { emulator.ejectCartridge(slot: 0); updateTitle() }
    @objc private func eject2() { emulator.ejectCartridge(slot: 1); updateTitle() }

    private func openCartridge(slot: Int) {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = Emulator.cartridgeExtensions.compactMap { UTType(filenameExtension: $0) }
        panel.message = "MSX 카트리지 ROM (.rom, .mx1, .zip)"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        insertCartridge(url, slot: slot)
    }

    private func insertCartridge(_ url: URL, slot: Int) {
        guard slot < emulator.machine.cartridgeSlots.count else {
            showError(NSError(domain: "KorMSX", code: 1, userInfo: [
                NSLocalizedDescriptionKey: "\(emulator.machine.name)에는 카트리지 슬롯 \(slot + 1)이 없습니다.",
            ]))
            return
        }
        do {
            try emulator.insertCartridge(url, slot: slot)
            NSDocumentController.shared.noteNewRecentDocumentURL(url)
        } catch {
            showError(error)
        }
        updateTitle()
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        if let url = urls.first { insertCartridge(url, slot: 0) }
    }

    @objc private func chooseROMFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.message = "MSX ROM이 들어 있는 폴더 (파일 이름은 상관없음, MAME zip도 가능)"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        romFolder = url
        do {
            try emulator.setROMLibrary(library)
            // Coming from the C-BIOS fallback: switch to a machine with BASIC if its ROMs are here now.
            if emulator.machine.bundled, let m = Machine.all.first(where: {
                guard !$0.bundled, case .success = library.images(for: $0) else { return false }
                return true
            }) {
                try emulator.switchMachine(to: m)
                defaults.set(m.id, forKey: "machine")
            }
        } catch {
            showError(error)
        }
        fitWindowToScreen()
        updateTitle()
    }

    @objc private func revealROMFolder() {
        // The chosen folder, else the ROMs inside the app package. Nothing is created.
        let candidates = [romFolder, ROMLibrary.bundledROMFolder].compactMap { $0 }
        if let dir = candidates.first(where: { FileManager.default.fileExists(atPath: $0.path) }) {
            NSWorkspace.shared.open(dir)
        } else {
            chooseROMFolder()
        }
    }

    @objc private func paste(_ sender: Any?) {
        if let text = NSPasteboard.general.string(forType: .string) { emulator.paste(text) }
    }

    @objc private func resetMachine() { emulator.reset() }
    @objc private func powerCycle() { emulator.powerCycle() }

    @objc private func togglePause() {
        paused.toggle()
        nextFrame = CACurrentMediaTime()
        updateTitle()
    }

    @objc private func selectMachine(_ sender: NSMenuItem) {
        let machine = Machine.all[sender.tag]
        do {
            try emulator.switchMachine(to: machine)
            defaults.set(machine.id, forKey: "machine")
        } catch {
            showError(error)
        }
        fitWindowToScreen()
        updateTitle()
    }

    /// Keeps the window's aspect ratio matched to the framebuffer (MSX1 288x224, MSX2 544x480).
    private func fitWindowToScreen() {
        let size = emulator.screenSize
        let aspect = NSSize(width: size.width, height: size.height)
        guard window.contentAspectRatio != aspect else { return }
        window.contentAspectRatio = aspect
        var frame = window.contentRect(forFrameRect: window.frame)
        let top = frame.maxY
        frame.size.height = frame.width * CGFloat(size.height) / CGFloat(size.width)
        frame.origin.y = top - frame.height
        window.setFrame(window.frameRect(forContentRect: frame), display: true)
    }

    @objc private func toggleCharacterMode() { screen.characterMode.toggle() }
    @objc private func toggleMute() { emulator.audio.muted.toggle() }

    private func showError(_ error: Error) {
        let alert = NSAlert()
        alert.messageText = "오류"
        alert.informativeText = error.localizedDescription
        alert.runModal()
        nextFrame = CACurrentMediaTime()
    }
}

extension AppDelegate: NSMenuDelegate {
    func menuNeedsUpdate(_ menu: NSMenu) {
        for item in menu.items {
            switch item.action {
            case #selector(selectMachine(_:)):
                item.state = Machine.all[item.tag].id == emulator.machine.id ? .on : .off
            case #selector(togglePause): item.state = paused ? .on : .off
            case #selector(toggleCharacterMode): item.state = screen.characterMode ? .on : .off
            case #selector(toggleMute): item.state = emulator.audio.muted ? .on : .off
            default: break
            }
        }
    }
}
