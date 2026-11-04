// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "KorMSX",
    platforms: [.macOS(.v13)],
    targets: [
        .target(
            name: "MSXCore",
            path: "Sources/MSXCore",
            cSettings: [.unsafeFlags(["-O3"])]
        ),
        .executableTarget(
            name: "KorMSX",
            dependencies: ["MSXCore"],
            path: "Sources/KorMSX",
            resources: [.copy("Resources/cbios")]
        ),
    ]
)
