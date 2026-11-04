import Foundation
import MSXCore

/// Finds ROM dumps in the ROM folders: plain files (any name, matched by
/// CRC32 or by MAME's file name), MAME-style machine subfolders, and zip
/// archives such as MAME's dpc200.zip.
struct ROMLibrary {
    let folders: [URL]

    /// Folders searched, in order: $KORMSX_ROMS, the user's chosen folder, the app's
    /// own Resources/roms, and roms/ next to the app and in the current directory.
    static func defaultFolders(userFolder: URL?) -> [URL] {
        var dirs: [URL] = []
        if let env = ProcessInfo.processInfo.environment["KORMSX_ROMS"] {
            dirs.append(URL(fileURLWithPath: env))
        }
        if let userFolder { dirs.append(userFolder) }
        if let bundled = bundledROMFolder { dirs.append(bundled) }
        let bundleDir = Bundle.main.bundleURL.deletingLastPathComponent()
        dirs.append(bundleDir.appendingPathComponent("roms"))
        dirs.append(URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent("roms"))
        return dirs
    }

    /// KorMSX.app/Contents/Resources/roms, filled by scripts/build_app.sh.
    static var bundledROMFolder: URL? {
        Bundle.main.resourceURL?.appendingPathComponent("roms")
    }

    private struct Candidate {
        let name: String
        let load: () -> Data?
        let size: Int
    }

    /// All candidate ROM images (at most 256 KB) in the folders, one level of subfolders, and zips.
    private func candidates() -> [Candidate] {
        let fm = FileManager.default
        var out: [Candidate] = []
        func scan(_ dir: URL, depth: Int) {
            guard let items = try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.fileSizeKey, .isDirectoryKey]) else { return }
            for url in items {
                let values = try? url.resourceValues(forKeys: [.fileSizeKey, .isDirectoryKey])
                if values?.isDirectory == true {
                    if depth == 0 { scan(url, depth: 1) }
                    continue
                }
                if url.pathExtension.lowercased() == "zip" {
                    for entry in Self.zipEntries(url) where entry.size <= 0x40000 {
                        out.append(Candidate(name: entry.name, load: { Self.unzip(url, entry: entry.name) }, size: entry.size))
                    }
                } else if let size = values?.fileSize, size > 0, size <= 0x40000 {
                    out.append(Candidate(name: url.lastPathComponent, load: { try? Data(contentsOf: url) }, size: size))
                }
            }
        }
        for dir in folders { scan(dir, depth: 0) }
        return out
    }

    /// Looks up each ROM of `machine`. Returns the images, or the labels of the missing ones.
    func images(for machine: Machine) -> Result<[Data], MissingROMs> {
        if machine.bundled {
            guard let dir = Self.bundledCBIOS else { return .failure(MissingROMs(labels: ["C-BIOS"])) }
            let datas = machine.roms.compactMap { try? Data(contentsOf: dir.appendingPathComponent($0.names[0])) }
            return datas.count == machine.roms.count ? .success(datas) : .failure(MissingROMs(labels: ["C-BIOS"]))
        }
        let all = candidates()
        var found: [Data] = []
        var missing: [String] = []
        for spec in machine.roms {
            // Prefer a CRC match among files of the right size, then a name match.
            var data: Data?
            for c in all where c.size == spec.size || (spec.size == 0x4000 && c.size == 0x2000) {
                guard let d = c.load() else { continue }
                if spec.crcs.contains(Self.crc(d)) { data = d; break }
            }
            if data == nil {
                for c in all where spec.names.contains(where: { $0.caseInsensitiveCompare(c.name) == .orderedSame }) {
                    if let d = c.load() { data = d; break }
                }
            }
            if let data { found.append(data) } else { missing.append("\(spec.label): \(spec.names[0])") }
        }
        return missing.isEmpty ? .success(found) : .failure(MissingROMs(labels: missing))
    }

    /// The bundled C-BIOS: in the .app's Resources, or in SwiftPM's resource
    /// bundle next to the executable (`swift run`). Bundle.module would trap
    /// when the bundle is missing, so it is looked up by hand.
    static let bundledCBIOS: URL? = {
        let exeDir = Bundle.main.executableURL?.deletingLastPathComponent()
        let candidates = [
            Bundle.main.resourceURL?.appendingPathComponent("cbios"),
            exeDir?.appendingPathComponent("KorMSX_KorMSX.bundle/Contents/Resources/cbios"),
            exeDir?.appendingPathComponent("KorMSX_KorMSX.bundle/cbios"),
        ]
        return candidates.compactMap { $0 }.first {
            FileManager.default.fileExists(atPath: $0.appendingPathComponent("cbios_main_msx1.rom").path)
        }
    }()

    struct MissingROMs: Error {
        let labels: [String]
    }

    static func crc(_ data: Data) -> UInt32 {
        data.withUnsafeBytes { msx_crc32($0.baseAddress, $0.count) }
    }

    // MARK: zip archives (through /usr/bin/unzip)

    struct ZipEntry {
        let name: String
        let size: Int
    }

    static func zipEntries(_ url: URL) -> [ZipEntry] {
        guard let listing = run("/usr/bin/unzip", ["-Z", "-l", url.path]),
              let text = String(data: listing, encoding: .utf8) else { return [] }
        // zipinfo -l lines: perms version os size type csize method date time name
        return text.split(separator: "\n").compactMap { line in
            let f = line.split(separator: " ", omittingEmptySubsequences: true)
            guard f.count >= 10, f[0].hasPrefix("-"), let size = Int(f[3]) else { return nil }
            return ZipEntry(name: f[9...].joined(separator: " "), size: size)
        }
    }

    static func unzip(_ url: URL, entry: String) -> Data? {
        run("/usr/bin/unzip", ["-p", url.path, entry])
    }

    private static func run(_ tool: String, _ args: [String]) -> Data? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return nil }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return p.terminationStatus == 0 ? data : nil
    }
}
