// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "SplatKit",
    platforms: [.iOS(.v17)],
    products: [.library(name: "SplatKit", targets: ["SplatKit"])],
    targets: [
        .binaryTarget(
            name: "SplatKitCore",
            url: "https://github.com/Xget7/splatkit-ios/releases/download/v0.1.0-beta.2/SplatKitCore.xcframework.zip",
            checksum: "a0bf4f27782195ba2e2b6e6d6a1dab0410e603d3feb07445691529e12f3ea2cc"
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
