// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "SplatKit",
    platforms: [.iOS(.v17)],
    products: [.library(name: "SplatKit", targets: ["SplatKit"])],
    targets: [
        .binaryTarget(
            name: "SplatKitCore",
            url: "https://github.com/Xget7/splatkit-ios/releases/download/v0.1.0-alpha.3/SplatKitCore.xcframework.zip",
            checksum: "2258de61db98721528a805bdfefbc6b764aada311bda6149b3abcf98d5194906"
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
