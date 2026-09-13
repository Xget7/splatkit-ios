import SwiftUI

@main
struct SplatKitDevApp: App {
    init() {
        // Configure the internal experiment before SplatSession creates its renderer.
        let enabled = LaunchArgs().bool("metal-culling") ?? false
        setenv("SPLATKIT_METAL_CULLING_EXPERIMENT", enabled ? "1" : "0", 1)
        let requestedRadius = LaunchArgs().float("min-pixel-radius") ?? 0.5
        let radius = requestedRadius.isFinite && requestedRadius >= 0 ? requestedRadius : 0.5
        setenv("SPLATKIT_METAL_MIN_PIXEL_RADIUS", String(radius), 1)
        let tileRaster = LaunchArgs().bool("tile-raster") ?? false
        setenv("SPLATKIT_METAL_TILE_RASTER", tileRaster ? "1" : "0", 1)
        let depthBits = LaunchArgs().string("depth-key-bits") ?? "32"
        setenv("SPLATKIT_METAL_DEPTH_KEY_BITS", depthBits, 1)
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .ignoresSafeArea()
                .statusBarHidden()
        }
    }
}
