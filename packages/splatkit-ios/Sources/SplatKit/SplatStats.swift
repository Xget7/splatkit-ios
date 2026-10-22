/// A snapshot of what the engine is doing, refreshed twice a second.
public struct SplatStats {
    /// Frames per second over the last half second: frames the display showed when
    /// `presentTiming` is true, otherwise frames submitted.
    public var fps: Float = 0
    public var frameMillis: Float = 0
    public var presentTiming = false
    /// 95th percentile display interval over the last 5 seconds; zero without present timing.
    public var frameMillisP95: Float = 0
    /// Frame rate of the slowest 1% of display intervals over the last 5 seconds.
    public var lowFps: Float = 0
    /// Submitted frames never shown in the last half second.
    public var droppedFrames: Int = 0
    /// GPU time of the last frame; zero until one completes.
    public var gpuMillis: Float = 0
    public var sortMillis: Float = 0
    public var splatCount: Int = 0
    /// Spherical harmonics degree the loaded world holds on the GPU, its file degree capped by
    /// `maxShDegree` at load; zero without a world. `shDegree` above it draws no more.
    public var loadedShDegree: Int = 0
    /// Splats in the loaded source world; not GPU residency for a streamed world.
    public var loadedSplatCount: Int { splatCount }
    /// Last completed visibility/order result, refreshed twice a second without a GPU wait.
    /// This counts submitted splats, not splats contributing a visible pixel after occlusion.
    public var drawnSplatCount: Int = 0
    public var walking = false
    public var motion = false

    public init() {}
}
