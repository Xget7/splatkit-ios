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

/// The walker in walk mode, in meters. The defaults are a standing adult: the eye 1.5 m over
/// the floor, 0.35 m kept from walls, and a 0.35 m rise walked onto, which climbs stairs and
/// doorsteps but not chairs or counters. A step onto anything higher is refused and the
/// walker slides along it instead.
public struct CharacterSettings: Equatable {
    public var eyeHeight: Float
    public var bodyRadius: Float
    public var stepHeight: Float

    public init(eyeHeight: Float = 1.5, bodyRadius: Float = 0.35, stepHeight: Float = 0.35) {
        self.eyeHeight = eyeHeight
        self.bodyRadius = bodyRadius
        self.stepHeight = stepHeight
    }
}

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

/// A view that renders with SplatKit, on a CAMetalLayer of its own.
///
/// The host forwards resume, pause and release. Everything else follows the view's
/// lifecycle: the engine gets the layer when the view is in a window and gives it back,
/// synchronously, before the view leaves it.
///
/// Touch: a drag looks around (yaw, and pitch when the gyroscope is off), and that can be
/// turned off. The view ships no walking control: the host draws its own, wherever it
/// likes, and drives `setWalkVelocity` or `walk` from it.
public final class SplatMetalView: UIView {
    public override class var layerClass: AnyClass { CAMetalLayer.self }

    private let renderThread = RenderThread()
    private lazy var motion = MotionInput { [weak self] in self?.renderThread.setAttitude($0) }
    private var motionEnabled = false
    private var resumed = false
    private var attached = false
    private var lastDrawableSize = CGSize.zero

    private lazy var touchLook = TouchLook(view: self) { [weak self] yaw, pitch in
        self?.renderThread.look(yaw, pitch)
    }
    private var poseTimer: Timer?
    private var lastPose: CameraPose?

    /// Radians per point dragged to look.
    public var lookSensitivity: Float {
        get { touchLook.sensitivity }
        set { touchLook.sensitivity = newValue }
    }
    /// Whether a drag on the view is allowed to turn the camera. A host that drives looking
    /// from its own control, and scripted tours, turn it off.
    public var touchLookEnabled: Bool {
        get { touchLook.isEnabled }
        set {
            touchLook.isEnabled = newValue
            if !newValue { touchLook.letGo() }
        }
    }
    /// How often the delegate hears where the camera is, in seconds; 0, the default, never.
    /// A pose is delivered only when it differs from the last one delivered.
    public var cameraPoseInterval: TimeInterval = 0 {
        didSet {
            cameraPoseInterval = max(cameraPoseInterval, 0)
            guard cameraPoseInterval != oldValue else { return }
            startPoseTimer()
        }
    }

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
        isMultipleTouchEnabled = true
        _ = touchLook
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

    /// Starts orbiting `point` from the current camera pose. Orbit is a temporary fly mode;
    /// first-person input resumes walk mode from the same pose.
    public func setAnchor(_ point: SIMD3<Float>) {
        renderThread.setAnchor(SKVec3(x: point.x, y: point.y, z: point.z))
    }

    /// Orbits around the anchor by radians. False until an anchor is available.
    @discardableResult
    public func orbit(deltaAzimuth: Float, deltaElevation: Float) -> Bool {
        renderThread.orbit(deltaAzimuth, deltaElevation)
    }

    /// Changes the orbit radius in metres. Positive values move away from the anchor.
    @discardableResult
    public func dolly(deltaRadius: Float) -> Bool {
        renderThread.dolly(deltaRadius)
    }

    /// Picks an anchor through normalized view coordinates. A collider must be loaded.
    /// A miss returns false and keeps the current anchor.
    @discardableResult
    public func focus(x: Float, y: Float) -> Bool {
        renderThread.focus(x, y)
    }

    /// Runs a finite azimuth turn in degrees at the average degrees per second. The engine
    /// keeps drawing while it moves and idles when it finishes.
    @discardableResult
    public func animateOrbit(
        degrees: Float,
        degreesPerSecond: Float,
        easeInOut: Bool = false
    ) -> Bool {
        renderThread.animateOrbit(degrees, degreesPerSecond, easeInOut)
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

    /// Walks continuously at the given speed in meters per second until called again with
    /// zeros: what a joystick or a keyboard drives. Forward is where the camera looks,
    /// flattened onto the floor while walking; right strafes.
    public func setWalkVelocity(forward: Float, right: Float) {
        renderThread.setVelocity(forward, right)
    }

    /// One step, in meters, for a host that integrates movement itself. The collider stops
    /// it at walls and the floor carries it, exactly as a velocity would.
    public func walk(forward: Float, right: Float) {
        renderThread.walk(forward, right)
    }

    /// Turns the camera by these radians: what a look pad or a mouse drives. Pitch is
    /// clamped, and ignored while the gyroscope drives the view.
    public func look(deltaYaw: Float, deltaPitch: Float) {
        renderThread.look(deltaYaw, deltaPitch)
    }

    /// The walker's shape in walk mode, applied at once and to a collider loaded later.
    /// False when a value is not a walkable one, and then the previous settings stay.
    @discardableResult
    public func setCharacter(_ settings: CharacterSettings) -> Bool {
        let accepted = renderThread.setCharacter(SKCharacterSettings(
            eyeHeight: settings.eyeHeight, bodyRadius: settings.bodyRadius,
            stepHeight: settings.stepHeight))
        if accepted { character = settings }
        return accepted
    }

    /// The walker's shape in effect.
    public private(set) var character = CharacterSettings()

    /// Runs a reproducible capture: the gyroscope goes off, the camera takes a fixed pose
    /// and turns once over `seconds`, then the frame time distribution is logged.
    public func startBenchmark(seconds: Float = 10) {
        setMotionEnabled(false)
        renderThread.startBenchmark(seconds)
    }

    /// GPU name and API reported by Metal.
    public var gpuDescription: String { renderThread.gpuDescription }

    /// The renderer policy for this view. Setting it re-validates on the render thread
    /// and keeps the previous policy when the request is invalid or preparation fails.
    public var renderPolicy: SKRenderPolicy {
        get { renderThread.renderPolicy }
        set { renderThread.applyRenderPolicy(newValue) }
    }

    /// Native limits, feature flags and the policy fields this device accepts.
    public var deviceCapabilities: SKDeviceCapabilities { renderThread.deviceCapabilities }

    /// True while the gyroscope drives the camera.
    public var isMotionEnabled: Bool { motionEnabled }

    /// Latest engine stats. Cheap; safe on the main thread.
    public func readStats() -> SplatStats {
        var stats = SplatStats()
        guard let s = renderThread.stats() else { return stats }
        stats.fps = s.fps
        stats.frameMillis = s.frameMillis
        stats.presentTiming = s.presentTiming.boolValue
        stats.frameMillisP95 = s.frameMillisP95
        stats.lowFps = s.lowFps
        stats.droppedFrames = Int(s.droppedFrames)
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
        startPoseTimer()
    }

    public func pause() {
        resumed = false
        motion.stop()
        renderThread.pause()
        stopPoseTimer()
    }

    public func release() {
        motion.stop()
        stopPoseTimer()
        detach()
        renderThread.release()
    }

    // Camera pose reporting.

    private func startPoseTimer() {
        stopPoseTimer()
        guard resumed, cameraPoseInterval > 0 else { return }
        let timer = Timer(timeInterval: cameraPoseInterval, repeats: true) { [weak self] _ in
            self?.reportPose()
        }
        // Common modes: a pose keeps arriving while a host's own control is being dragged.
        RunLoop.main.add(timer, forMode: .common)
        poseTimer = timer
    }

    private func stopPoseTimer() {
        poseTimer?.invalidate()
        poseTimer = nil
    }

    private func reportPose() {
        guard let delegate else { return }
        let pose = cameraPose
        guard pose != lastPose else { return }
        lastPose = pose
        delegate.splatView(self, cameraPoseChanged: pose)
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
        touchLook.letGo()
        guard attached else { return }
        attached = false
        renderThread.layerDetached()
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
