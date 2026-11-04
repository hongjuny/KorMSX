import Foundation
import MSXCore

/// Keyboard matrix layout of a machine (which symbols sit on which keys).
enum KeyboardLayout {
    case international
    case korean     // MAME msxkr: the Japanese layout with ₩ in place of ¥ and a 한글 key for CODE
}

/// One ROM dump of a machine. Dumps are found by CRC32 whatever their file
/// name, or by one of `names` (MAME's file names first).
struct ROMFile {
    let label: String
    let names: [String]
    let crcs: [UInt32]
    let size: Int
}

/// Where part of a ROM file appears: MAME's add_internal_slot(prim, sec, page, numpages, region, offset).
struct Placement {
    let file: Int
    var offset = 0
    let prim: Int, sec: Int, page: Int, pages: Int
}

enum RAMLayout {
    case plain(prim: Int, sec: Int, page: Int, pages: Int)
    case mapper(prim: Int, sec: Int, kb: Int)
}

/// A machine layout, transcribed from MAME's msx1.cpp / msx2.cpp definitions.
struct Machine: Identifiable {
    let id: String              // MAME short name
    let name: String
    let generation: String      // "MSX1", "MSX2", "MSX2+"
    let roms: [ROMFile]
    let placements: [Placement]
    let ram: RAMLayout
    var expandedSlots: [Int] = []
    /// Primary slots of the cartridge slots (MAME's add_cartridge_slot<n>).
    let cartridgeSlots: [Int]
    let layout: KeyboardLayout
    var vdp: msx_vdp_type = MSX_VDP_TMS9918
    /// Index in `roms` of the kanji font ROM (ports D8h-DBh), if any.
    var kanji: Int?
    /// Uses the bundled C-BIOS instead of user ROMs.
    var bundled = false
    /// Hardware of the real machine that is not emulated yet.
    var missing: String?
}

extension Machine {
    // MARK: MSX1

    /// The common Korean MSX1 BIOS shared by Daewoo, Goldstar and Samsung machines.
    private static func koreanBIOS(_ name: String) -> ROMFile {
        ROMFile(label: "BIOS (32 KB)", names: [name, "kor_bios.rom"], crcs: [0x3ab0cd3b], size: 0x8000)
    }

    /// Qnix's Hangul BASIC extension ROM.
    private static func hangul(_ name: String, crc: UInt32, size: Int) -> ROMFile {
        ROMFile(label: "한글 ROM (\(size / 1024) KB)", names: [name, "kor_hangul.rom"], crcs: [crc], size: size)
    }

    /// Korean MSX1: BIOS in slot 0 pages 0-1, hangul ROM in slot 0 page 2, RAM in slot 2, cartridge slot 1.
    private static func koreanMSX1(_ id: String, _ name: String, bios: ROMFile, han: ROMFile) -> Machine {
        Machine(id: id, name: name, generation: "MSX1", roms: [bios, han],
                placements: [Placement(file: 0, prim: 0, sec: 0, page: 0, pages: 2),
                             Placement(file: 1, prim: 0, sec: 0, page: 2, pages: 1)],
                ram: .plain(prim: 2, sec: 0, page: 0, pages: 4), cartridgeSlots: [1], layout: .korean)
    }

    // MARK: MSX2

    /// Daewoo MSX2 layout (MAME cpc300/cpc400): slot 0 expanded into BIOS, hangul,
    /// 128 KB mapper RAM and sub ROM; cartridge slot 1.
    private static let daewooMSX2RAM = RAMLayout.mapper(prim: 0, sec: 2, kb: 128)

    static let all: [Machine] = [
        Machine(id: "cbios", name: "C-BIOS MSX1 (내장, 카트리지 전용)", generation: "MSX1",
                roms: [ROMFile(label: "C-BIOS main", names: ["cbios_main_msx1.rom"], crcs: [], size: 0x8000),
                       ROMFile(label: "C-BIOS logo", names: ["cbios_logo_msx1.rom"], crcs: [], size: 0x4000)],
                placements: [Placement(file: 0, prim: 0, sec: 0, page: 0, pages: 2),
                             Placement(file: 1, prim: 0, sec: 0, page: 2, pages: 1)],
                ram: .plain(prim: 3, sec: 0, page: 0, pages: 4), cartridgeSlots: [1, 2], layout: .international,
                bundled: true),

        // The generic international (European, 50 Hz) BIOS + BASIC that blueMSX and
        // openMSX ship as MSX.ROM; laid out like a typical MSX1 with RAM in slot 3.
        Machine(id: "msx1int", name: "MSX1 국제판 (MSX.ROM)", generation: "MSX1",
                roms: [ROMFile(label: "BIOS+BASIC (32 KB)", names: ["MSX.ROM"], crcs: [0x94ee12f3], size: 0x8000)],
                placements: [Placement(file: 0, prim: 0, sec: 0, page: 0, pages: 2)],
                ram: .plain(prim: 3, sec: 0, page: 0, pages: 4), cartridgeSlots: [1, 2], layout: .international),

        koreanMSX1("dpc200", "Daewoo IQ-1000 DPC-200",
                   bios: koreanBIOS("200bios.rom"), han: hangul("200han.rom", crc: 0x97478efb, size: 0x4000)),
        koreanMSX1("gsfc80u", "Goldstar FC-80U",
                   bios: koreanBIOS("fc80ubios.rom"), han: hangul("fc80uhan.rom", crc: 0x0cdb8501, size: 0x2000)),
        koreanMSX1("spc800", "Samsung SPC-800",
                   bios: koreanBIOS("spc800bios.rom"), han: hangul("spc800han.rom", crc: 0x5ae2b013, size: 0x4000)),

        Machine(id: "gfc1080", name: "Goldstar GFC-1080", generation: "MSX1",
                roms: [ROMFile(label: "BIOS (32 KB)", names: ["gfc1080bios.rom"], crcs: [0xd9cdd4a6], size: 0x8000),
                       hangul("gfc1080han.rom", crc: 0xf209448c, size: 0x4000),
                       ROMFile(label: "Pasocalc", names: ["gfc1080pasocalc.rom"], crcs: [0x4014f7ea], size: 0x4000)],
                placements: [Placement(file: 0, prim: 0, sec: 0, page: 0, pages: 2),
                             Placement(file: 1, prim: 0, sec: 0, page: 2, pages: 1),
                             Placement(file: 2, prim: 0, sec: 0, page: 3, pages: 1)],
                ram: .plain(prim: 2, sec: 0, page: 0, pages: 4), cartridgeSlots: [1], layout: .korean),

        Machine(id: "cpc51", name: "Daewoo Zemmix CPC-51", generation: "MSX1",
                roms: [ROMFile(label: "BIOS (32 KB)", names: ["cpc-51_v_1_01.ic05"], crcs: [0xc3a868ef], size: 0x8000)],
                placements: [Placement(file: 0, prim: 0, sec: 0, page: 0, pages: 2)],
                ram: .plain(prim: 2, sec: 0, page: 0, pages: 4), cartridgeSlots: [1], layout: .korean),

        // MARK: MSX2 / MSX2+

        Machine(id: "cbios2", name: "C-BIOS MSX2 (내장, 카트리지 전용)", generation: "MSX2",
                roms: [ROMFile(label: "C-BIOS main", names: ["cbios_main_msx2.rom"], crcs: [], size: 0x8000),
                       ROMFile(label: "C-BIOS logo", names: ["cbios_logo_msx2.rom"], crcs: [], size: 0x4000),
                       ROMFile(label: "C-BIOS sub", names: ["cbios_sub.rom"], crcs: [], size: 0x4000)],
                placements: [Placement(file: 0, prim: 0, sec: 0, page: 0, pages: 2),
                             Placement(file: 1, prim: 0, sec: 0, page: 2, pages: 1),
                             Placement(file: 2, prim: 3, sec: 0, page: 0, pages: 1)],
                ram: .mapper(prim: 3, sec: 2, kb: 512), expandedSlots: [3], cartridgeSlots: [1, 2],
                layout: .international, vdp: MSX_VDP_V9938, bundled: true),

        Machine(id: "cbios2p", name: "C-BIOS MSX2+ (내장, 카트리지 전용)", generation: "MSX2+",
                roms: [ROMFile(label: "C-BIOS main", names: ["cbios_main_msx2+.rom"], crcs: [], size: 0x8000),
                       ROMFile(label: "C-BIOS logo", names: ["cbios_logo_msx2+.rom"], crcs: [], size: 0x4000),
                       ROMFile(label: "C-BIOS sub", names: ["cbios_sub.rom"], crcs: [], size: 0x4000)],
                placements: [Placement(file: 0, prim: 0, sec: 0, page: 0, pages: 2),
                             Placement(file: 1, prim: 0, sec: 0, page: 2, pages: 1),
                             Placement(file: 2, prim: 3, sec: 0, page: 0, pages: 1)],
                ram: .mapper(prim: 3, sec: 2, kb: 512), expandedSlots: [3], cartridgeSlots: [1, 2],
                layout: .international, vdp: MSX_VDP_V9958, bundled: true),

        // Philips NMS 8250 layout (MAME nms8250), whose main BIOS is the common MSX2.ROM.
        Machine(id: "nms8250", name: "MSX2 국제판 (Philips NMS 8250 구성)", generation: "MSX2",
                roms: [ROMFile(label: "MSX2 BIOS+BASIC (32 KB)", names: ["d23c256eac.ic119", "MSX2.ROM"],
                               crcs: [0x6cdaf3a5], size: 0x8000),
                       ROMFile(label: "MSX2 서브 ROM (16 KB)", names: ["d23128ec.ic118", "MSX2EXT.ROM"],
                               crcs: [0x66237ecf], size: 0x4000)],
                placements: [Placement(file: 0, prim: 0, sec: 0, page: 0, pages: 2),
                             Placement(file: 1, prim: 3, sec: 0, page: 0, pages: 4)],
                ram: .mapper(prim: 3, sec: 2, kb: 128), expandedSlots: [3], cartridgeSlots: [1, 2],
                layout: .international, vdp: MSX_VDP_V9938, missing: "디스크 드라이브"),

        Machine(id: "cpc300", name: "Daewoo IQ-2000 CPC-300", generation: "MSX2",
                roms: [ROMFile(label: "BIOS (32 KB)", names: ["300bios.rom"], crcs: [0x53850907], size: 0x8000),
                       ROMFile(label: "서브 ROM (32 KB)", names: ["300ext.rom"], crcs: [0xd64da39c], size: 0x8000),
                       ROMFile(label: "한글 ROM (32 KB)", names: ["300han.rom"], crcs: [0xe78cd87f], size: 0x8000)],
                placements: [Placement(file: 0, prim: 0, sec: 0, page: 0, pages: 2),
                             Placement(file: 2, prim: 0, sec: 1, page: 1, pages: 2),
                             Placement(file: 1, prim: 0, sec: 3, page: 0, pages: 2)],
                ram: daewooMSX2RAM, expandedSlots: [0], cartridgeSlots: [1], layout: .korean,
                vdp: MSX_VDP_V9938, missing: "S-1985 MSX 엔진의 백업 RAM"),

        Machine(id: "cpc400", name: "Daewoo X-II CPC-400", generation: "MSX2",
                roms: [ROMFile(label: "BIOS (32 KB)", names: ["400bios.rom"], crcs: [0x53850907], size: 0x8000),
                       ROMFile(label: "서브 ROM (32 KB)", names: ["400ext.rom"], crcs: [0x2ba104a3], size: 0x8000),
                       ROMFile(label: "한글 ROM (32 KB)", names: ["400han.rom"], crcs: [0xa8ead5e3], size: 0x8000),
                       ROMFile(label: "한자 폰트 ROM (128 KB)", names: ["400kfn.rom"], crcs: [0xb663c605], size: 0x20000)],
                placements: [Placement(file: 0, prim: 0, sec: 0, page: 0, pages: 2),
                             Placement(file: 2, prim: 0, sec: 1, page: 1, pages: 2),
                             Placement(file: 1, prim: 0, sec: 3, page: 0, pages: 2)],
                ram: daewooMSX2RAM, expandedSlots: [0], cartridgeSlots: [1], layout: .korean,
                vdp: MSX_VDP_V9938, kanji: 3, missing: "디스크 드라이브"),

        // CPC-400S packs BIOS, disk and sub ROM into one 64 KB ROM, and the hangul
        // driver (two 16 KB parts) with the kanji driver into another.
        Machine(id: "cpc400s", name: "Daewoo X-II CPC-400S", generation: "MSX2",
                roms: [ROMFile(label: "BIOS+디스크+서브 ROM (64 KB)", names: ["400sbios.u38"], crcs: [0xdbb0f26d], size: 0x10000),
                       ROMFile(label: "한글+한자 드라이버 ROM (64 KB)", names: ["400shan.u44"], crcs: [0x5fdb493c], size: 0x10000),
                       ROMFile(label: "한자 폰트 ROM (128 KB)", names: ["400skfn.rom"], crcs: [0xfa85368c], size: 0x20000)],
                placements: [Placement(file: 0, prim: 0, sec: 0, page: 0, pages: 2),
                             Placement(file: 1, offset: 0x4000, prim: 0, sec: 1, page: 1, pages: 1),
                             Placement(file: 1, offset: 0xc000, prim: 0, sec: 1, page: 2, pages: 1),
                             Placement(file: 0, offset: 0xc000, prim: 0, sec: 3, page: 0, pages: 1),
                             Placement(file: 1, offset: 0x0000, prim: 0, sec: 3, page: 1, pages: 1)],
                ram: daewooMSX2RAM, expandedSlots: [0], cartridgeSlots: [1], layout: .korean,
                vdp: MSX_VDP_V9938, kanji: 2, missing: "디스크 드라이브"),
    ]
}
