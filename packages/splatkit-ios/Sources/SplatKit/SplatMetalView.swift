import QuartzCore
import UIKit
#if canImport(SplatKitCore)
import SplatKitCore
#endif

/// Where the camera is and where it looks: position in the world's frame in meters, yaw
/// about the up axis and pitch, both in radians. Pitch is clamped to 85 degrees. Read it
/// to save a viewpoint, set it to teleport or restore one.
public struct CameraPose: Equatable {
    public var x: Float
    public var y: Float
    public var z: Float
    public var yaw: Float
    public var pitch: Float

    public init(x: Float, y: Float, z: Float, yaw: Float = 0, pitch: Float = 0) {
        self.x = x
        self.y = y
        self.z = z
        self.yaw = yaw
        self.pitch = pitch
    }
}

/// A snapshot of what the engine is doing, refreshed twice a second.
public struct SplatStats {
    public var fps: Float = 0
    public var frameMillis: Float = 0
    /// GPU time of the last frame; zero until one completes.
    public var gpuMillis: Float = 0
    public var sortMillis: Float = 0
    public var splatCount: Int = 0
    /// Splats in the loaded source world; not GPU residency for a streamed world.
    public var loadedSplatCount: Int { splatCount }
    /// Last completed visibility/order result, refreshed twice a second without a GPU wait.
    /// This counts submitted splats, not splats contributing a visible pixel after occlusion.
    public var drawnSplatCount: Int = 0
    /// Screen tiles completed by compute, including background-only tiles.
    /// All tile counts are zero when hybrid diagnostics are unavailable.
    public var computeTileCount: Int = 0
    /// Compute tiles whose candidate list contains at least one splat.
    public var nonemptyComputeTileCount: Int = 0
    public var hardwareTileCount: Int = 0
    public var walking = false
    public var motion = false

    public init() {}
}

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
}

public extension SplatViewDelegate {
    func splatView(_ view: SplatMetalView, worldReady splatCount: Int) {}
    func splatView(_ view: SplatMetalView, worldFrameReady splatCount: Int) {}
    func splatView(_ view: SplatMetalView, worldFailed message: String) {}
    func splatViewColliderReady(_ view: SplatMetalView) {}
    func splatView(_ view: SplatMetalView, colliderFailed message: String) {}
}

/// A view that renders with SplatKit, on a CAMetalLayer of its own.
///
/// The host forwards resume, pause and release. Everything else follows the view's
/// lifecycle: the engine gets the layer when the view is in a window and gives it back,
/// synchronously, before the view leaves it.
///
/// Gestures: one finger drags the view (yaw, and pitch when the gyroscope is off);
/// two fingers walk (up is forward, sideways strafes); a double tap toggles the gyroscope.
public final class SplatMetalView: UIView {
    public override class var layerClass: AnyClass { CAMetalLayer.self }

    private let renderThread = RenderThread()
    private lazy var motion = MotionInput { [weak self] in self?.renderThread.setAttitude($0) }
    private var motionEnabled = false
    private var resumed = false
    private var attached = false
    private var lastDrawableSize = CGSize.zero

    /// Radians per point dragged.
    public var lookSensitivity: Float = 0.004
    /// Meters per point dragged with two fingers.
    public var walkSensitivity: Float = 0.01
    /// Whether a one-finger drag is allowed to move the camera. Scripted tours can disable it.
    public var touchLookEnabled = true
    /// Whether the double-tap gesture can toggle motion input.
    public var motionToggleEnabled = true

    public weak var delegate: SplatViewDelegate?

    public override init(frame: CGRect) {
        super.init(frame: frame)
        setUp()
    }

    public required init?(coder: NSCoder) {
        super.init(coder: coder)
        setUp()
    }

    private func setUp() {
        isOpaque = true
        backgroundColor = .black
        renderThread.onEvent = { [weak self] event, message, count in
            guard let self, let delegate = self.delegate else { return }
            switch event {
            case .worldReady: delegate.splatView(self, worldReady: Int(count))
            case .worldFrameReady: delegate.splatView(self, worldFrameReady: Int(count))
            case .worldFailed: delegate.splatView(self, worldFailed: message)
            case .colliderReady: delegate.splatViewColliderReady(self)
            case .colliderFailed: delegate.splatView(self, colliderFailed: message)
            @unknown default: break
            }
        }
        let look = UIPanGestureRecognizer(target: self, action: #selector(onLook(_:)))
        look.maximumNumberOfTouches = 1
        addGestureRecognizer(look)
        let walk = UIPanGestureRecognizer(target: self, action: #selector(onWalk(_:)))
        walk.minimumNumberOfTouches = 2
        walk.maximumNumberOfTouches = 2
        addGestureRecognizer(walk)
        let tap = UITapGestureRecognizer(target: self, action: #selector(onDoubleTap))
        tap.numberOfTapsRequired = 2
        addGestureRecognizer(tap)
    }

    /// False when Metal could not be brought up on this device; the view stays blank.
    public var isAvailable: Bool { renderThread.isAvailable }

    /// Decodes and shows a world from a file the app can read. The file is mapped, not
    /// copied, so this is the way to load big worlds. Replaces the current one when ready.
    public func loadWorld(file: URL) { renderThread.loadWorldFile(file.path) }

    /// Decodes a collider GLB from a file; enables walk mode when ready.
    public func loadCollider(file: URL) { renderThread.loadColliderFile(file.path) }

    /// Shows a tiled world from its index, a `tileset.json` with its tiles beside it (made
    /// offline by `splat-tile`). Only the index is read now; tiles stream in as the camera
    /// needs them, nearest and biggest on screen first, within `residencyBudget`.
    public func loadTiledWorld(tileset: URL) { renderThread.loadTiledWorldFile(tileset.path) }

    /// The camera's position and look direction, as of the last frame when read; setting
    /// teleports, and when walking the camera settles on the floor under the new point.
    public var cameraPose: CameraPose {
        get {
            guard let p = renderThread.cameraPose() else { return CameraPose(x: 0, y: 0, z: 0) }
            return CameraPose(x: p.x, y: p.y, z: p.z, yaw: p.yaw, pitch: p.pitch)
        }
        set { renderThread.setCameraPose(SKCameraPose(x: newValue.x, y: newValue.y, z: newValue.z, yaw: newValue.yaw, pitch: newValue.pitch)) }
    }

    /// Scripted camera: teleports to `position` looking at `target`, with `up` at the top
    /// of the frame whatever the roll, so a path can pass over the poles that yaw and
    /// pitch cannot. The next touch or motion update takes the view back.
    public func lookAt(from position: SIMD3<Float>, target: SIMD3<Float>, up: SIMD3<Float>) {
        renderThread.lookAt(from: SKVec3(x: position.x, y: position.y, z: position.z),
                            target: SKVec3(x: target.x, y: target.y, z: target.z),
                            up: SKVec3(x: up.x, y: up.y, z: up.z))
    }

    /// Fraction of the view's resolution the splats are drawn at, in [0.1, 2]. Below one
    /// the frame is drawn smaller and upscaled; above one it is supersampled.
    public var renderScale: Float = 1 {
        didSet {
            renderScale = min(max(renderScale, 0.1), 2)
            renderThread.setRenderScale(renderScale)
        }
    }

    /// Angular margin around the view, in degrees, kept drawn so that what turns into
    /// view before the next cull lands is already there. 10 by default.
    public var cullMarginDegrees: Float = 10 {
        didSet {
            cullMarginDegrees = min(max(cullMarginDegrees, 0), 80)
            renderThread.setCullMargin(cullMarginDegrees)
        }
    }

    /// Blend splats in linear light instead of the encoded colour space the training
    /// used. Off by default.
    public var linearBlending = false {
        didSet { renderThread.setLinearBlending(linearBlending) }
    }

    /// Most splats drawn per frame for a single file world, or 0 to draw them all. With a
    /// budget, a world loaded afterwards gets a level of detail hierarchy.
    public var splatBudget = 0 {
        didSet {
            splatBudget = max(splatBudget, 0)
            renderThread.setSplatBudget(splatBudget)
        }
    }

    /// Residency budget of a tiled world: the most splats held on the GPU at once, about
    /// 32 bytes each plus the harmonics. Applies to tiled worlds loaded after it is set.
    public var residencyBudget = 2_000_000 {
        didSet {
            residencyBudget = min(max(residencyBudget, 100_000), 32_000_000)
            renderThread.setResidencyBudget(residencyBudget)
        }
    }

    /// Spherical harmonics degree drawn, 0 to 3, capped by what the loaded world carries.
    public var shDegree = 3 {
        didSet {
            shDegree = min(max(shDegree, 0), 3)
            renderThread.setShDegree(shDegree)
        }
    }

    /// Highest spherical harmonics degree decoded and kept in GPU memory from the file, 0 to 3,
    /// applied to worlds loaded after it is set. The source file remains complete.
    public var maxShDegree = 3 {
        didSet {
            maxShDegree = min(max(maxShDegree, 0), 3)
            renderThread.setMaxShDegree(maxShDegree)
        }
    }

    /// Walks continuously at the given speed in meters per second until called again with zeros.
    public func setWalkVelocity(forward: Float, right: Float) {
        renderThread.setVelocity(forward, right)
    }

    /// Runs a reproducible capture: the gyroscope goes off, the camera takes a fixed pose
    /// and turns once over `seconds`, then the frame time distribution is logged.
    public func startBenchmark(seconds: Float = 10) {
        setMotionEnabled(false)
        renderThread.startBenchmark(seconds)
    }

    /// GPU name and API reported by Metal.
    public var gpuDescription: String { renderThread.gpuDescription }

    /// True while the gyroscope drives the camera.
    public var isMotionEnabled: Bool { motionEnabled }

    /// Latest engine stats. Cheap; safe on the main thread.
    public func readStats() -> SplatStats {
        var stats = SplatStats()
        guard let s = renderThread.stats() else { return stats }
        stats.fps = s.fps
        stats.frameMillis = s.frameMillis
        stats.gpuMillis = s.gpuMillis
        stats.sortMillis = s.sortMillis
        stats.splatCount = Int(s.splatCount)
        stats.drawnSplatCount = Int(s.drawnSplatCount)
        stats.computeTileCount = Int(s.computeTileCount)
        stats.nonemptyComputeTileCount = Int(s.nonemptyComputeTileCount)
        stats.hardwareTileCount = Int(s.hardwareTileCount)
        stats.walking = s.walking.boolValue
        stats.motion = s.motion.boolValue
        return stats
    }

    /// Drives the camera with the phone's orientation. No-op when the sensor is missing.
    public func setMotionEnabled(_ enabled: Bool) {
        motionEnabled = enabled && motion.isAvailable
        renderThread.setMotionEnabled(motionEnabled)
        if resumed {
            if motionEnabled { motion.start() } else { motion.stop() }
        }
    }

    public func resume() {
        resumed = true
        motion.interfaceOrientation = interfaceOrientation
        renderThread.resume()
        if motionEnabled { motion.start() }
    }

    public func pause() {
        resumed = false
        motion.stop()
        renderThread.pause()
    }

    public func release() {
        motion.stop()
        detach()
        renderThread.release()
    }

    // Layer lifecycle.

    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    private var interfaceOrientation: UIInterfaceOrientation {
        window?.windowScene?.interfaceOrientation ?? .portrait
    }

    private var drawableSize: CGSize {
        let scale = window?.screen.scale ?? UIScreen.main.scale
        return CGSize(width: (bounds.width * scale).rounded(), height: (bounds.height * scale).rounded())
    }

    public override func willMove(toWindow newWindow: UIWindow?) {
        if newWindow == nil { detach() }
        super.willMove(toWindow: newWindow)
    }

    public override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil { attachIfSized() }
    }

    public override func layoutSubviews() {
        super.layoutSubviews()
        metalLayer.contentsScale = window?.screen.scale ?? UIScreen.main.scale
        let size = drawableSize
        guard size.width > 0, size.height > 0 else { return }
        metalLayer.drawableSize = size
        motion.interfaceOrientation = interfaceOrientation
        if !attached {
            attachIfSized()
        } else if size != lastDrawableSize {
            lastDrawableSize = size
            renderThread.layerResized(size)
        }
    }

    private func attachIfSized() {
        let size = drawableSize
        guard !attached, window != nil, size.width > 0, size.height > 0 else { return }
        metalLayer.drawableSize = size
        lastDrawableSize = size
        attached = true
        renderThread.layerAttached(metalLayer, size: size)
    }

    private func detach() {
        guard attached else { return }
        attached = false
        renderThread.layerDetached()
    }

    // Gestures.

    @objc private func onLook(_ g: UIPanGestureRecognizer) {
        guard touchLookEnabled else {
            g.setTranslation(.zero, in: self)
            return
        }
        let d = g.translation(in: self)
        renderThread.look(-Float(d.x) * lookSensitivity, -Float(d.y) * lookSensitivity)
        g.setTranslation(.zero, in: self)
    }

    @objc private func onWalk(_ g: UIPanGestureRecognizer) {
        let d = g.translation(in: self)
        renderThread.walk(-Float(d.y) * walkSensitivity, Float(d.x) * walkSensitivity)
        g.setTranslation(.zero, in: self)
    }

    @objc private func onDoubleTap() {
        guard motionToggleEnabled else { return }
        setMotionEnabled(!motionEnabled)
    }
}

public extension SplatMetalView {
    /// Saves the next frame as a PNG at the view's pixel resolution. `completion` runs on
    /// the main thread.
    func captureFrame(to file: URL, completion: @escaping (Bool) -> Void) {
        renderThread.captureFrame(file.path) { ok in
            DispatchQueue.main.async { completion(ok) }
        }
    }
}
