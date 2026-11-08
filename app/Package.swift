// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "EIEPro",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(
            name: "EIEPro",
            path: "Sources/EIEPro",
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("SwiftUI"),
                .linkedFramework("CoreAudio"),
                .linkedFramework("CoreMIDI"),
                .linkedFramework("ServiceManagement"),
            ]
        )
    ]
)
