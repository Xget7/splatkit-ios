// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "SplatKit",
    platforms: [.iOS(.v17)],
    products: [.library(name: "SplatKit", targets: ["SplatKit"])],
    targets: [
        .binaryTarget(
            name: "SplatKitCore",
            url: "https://github.com/Xget7/splatkit-ios/releases/download/v0.1.0-alpha.5/SplatKitCore.xcframework.zip",
            checksum: "89bc7b1f436949bce77df2703a2b46fcd434e2f0bf412ec6c8811439aa0cdc4b"
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
