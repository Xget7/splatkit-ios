// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "SplatKit",
    platforms: [.iOS(.v17)],
    products: [.library(name: "SplatKit", targets: ["SplatKit"])],
    targets: [
        .binaryTarget(
            name: "SplatKitCore",
            url: "https://github.com/Xget7/splatkit-ios/releases/download/v0.1.0-beta.3/SplatKitCore.xcframework.zip",
            checksum: "a8f902652102299e1ed96f63c7c737666a9e2527a04dc8bfbe4ae465a9450cc2"
        ),
        .target(
            name: "SplatKit",
            dependencies: ["SplatKitCore"],
            path: "packages/splatkit-ios/Sources/SplatKit",
            linkerSettings: [
                .linkedLibrary("c++"), .linkedLibrary("z"),
                .linkedFramework("Metal"), .linkedFramework("QuartzCore"),
                .linkedFramework("Foundation"), .linkedFramework("CoreGraphics"),
                .linkedFramework("ImageIO"), .linkedFramework("UniformTypeIdentifiers"),
            ]
        ),
    ]
)
