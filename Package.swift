// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "SplatKit",
    platforms: [.iOS(.v17)],
    products: [.library(name: "SplatKit", targets: ["SplatKit"])],
    targets: [
        .binaryTarget(
            name: "SplatKitCore",
            url: "https://github.com/Xget7/splatkit-ios/releases/download/v0.1.0-alpha.2/SplatKitCore.xcframework.zip",
            checksum: "477f26f4ac6c70a42c56209ae87450b2adefa5bf32cd54059148566944ec71e7"
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
