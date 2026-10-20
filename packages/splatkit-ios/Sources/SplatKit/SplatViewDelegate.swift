/// Loading outcomes, delivered on the main thread.
public protocol SplatViewDelegate: AnyObject {
    /// The world is uploaded; its first GPU frame may still be pending.
    func splatView(_ view: SplatMetalView, worldReady splatCount: Int)
    /// First successful GPU frame of this uploaded world has completed. Suitable for
    /// dismissing a loading cover or starting a tour. Requires resume(), even while
    /// covered. Not a guarantee that streamed tiles are resident or LOD detail is exact.
    func splatView(_ view: SplatMetalView, worldFrameReady splatCount: Int)
    /// The file was not a readable world, or the GPU refused it; the previous world stays.
    func splatView(_ view: SplatMetalView, worldFailed message: String)
    /// Walk mode is on.
    func splatViewColliderReady(_ view: SplatMetalView)
    func splatView(_ view: SplatMetalView, colliderFailed message: String)
    /// Where the camera ended up, at most once per `cameraPoseInterval` and only while it
    /// moves. Off until that interval is set.
    func splatView(_ view: SplatMetalView, cameraPoseChanged pose: CameraPose)
}

public extension SplatViewDelegate {
    func splatView(_ view: SplatMetalView, worldReady splatCount: Int) {}
    func splatView(_ view: SplatMetalView, worldFrameReady splatCount: Int) {}
    func splatView(_ view: SplatMetalView, worldFailed message: String) {}
    func splatViewColliderReady(_ view: SplatMetalView) {}
    func splatView(_ view: SplatMetalView, colliderFailed message: String) {}
    func splatView(_ view: SplatMetalView, cameraPoseChanged pose: CameraPose) {}
}
