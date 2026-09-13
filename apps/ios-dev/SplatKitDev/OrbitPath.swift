import QuartzCore
import simd

/// Constant-distance inspection orbit. The camera moves; its field of view never changes.
final class OrbitPath {
    struct Settings {
        var pivot: SIMD3<Float>
        var radius: Float = 45
        var degreesPerSecond: Float = 4
        var startDegrees: Float = 90
        var horizontal: Bool = true
    }

    // CADisplayLink retains its target. A weak proxy lets the session release this path.
    private final class TickTarget: NSObject {
        weak var owner: OrbitPath?
        @objc func tick(_ link: CADisplayLink) { owner?.tick(link) }
    }

    private let view: SplatMetalView
    private let settings: Settings
    private let target = TickTarget()
    private var link: CADisplayLink?
    private var lastTimestamp: CFTimeInterval?
    private var angle: Double

    init(view: SplatMetalView, settings: Settings) {
        self.view = view
        self.settings = settings
        angle = Double(settings.startDegrees) * .pi / 180
        target.owner = self
        NSLog("SplatOrbit: axis=X horizontal=%d radius=%.2f speed=%.2f start=%.2f",
              settings.horizontal ? 1 : 0, settings.radius, settings.degreesPerSecond, settings.startDegrees)
        // Prepare the requested starting pose without advancing time during loading.
        applyPose()
    }

    deinit { end() }

    func begin() {
        guard link == nil else { return }
        lastTimestamp = nil
        applyPose()
        let link = CADisplayLink(target: target, selector: #selector(TickTarget.tick(_:)))
        link.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: 120, preferred: 120)
        link.add(to: .main, forMode: .common)
        self.link = link
    }

    func end() {
        link?.invalidate()
        link = nil
        lastTimestamp = nil
    }

    private func tick(_ link: CADisplayLink) {
        if let previous = lastTimestamp {
            angle += (link.timestamp - previous) * Double(settings.degreesPerSecond) * .pi / 180
            angle = angle.truncatingRemainder(dividingBy: 2 * .pi)
        }
        lastTimestamp = link.timestamp
        applyPose()
    }

    private func applyPose() {
        // Keep the same YZ orbit around the ISS's long X axis. A tangent up vector
        // rolls the camera 90 degrees, keeping that axis horizontal at every angle.
        // Unlike fixed world-Y up, it never becomes parallel to the viewing ray.
        let outward = SIMD3<Float>(0, Float(cos(angle)), Float(sin(angle)))
        let axis = SIMD3<Float>(1, 0, 0)
        let up = settings.horizontal ? simd_cross(outward, axis) : axis
        view.lookAt(from: settings.pivot + settings.radius * outward,
                    target: settings.pivot, up: up)
    }
}
