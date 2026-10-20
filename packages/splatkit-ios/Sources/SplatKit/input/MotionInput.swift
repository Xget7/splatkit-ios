import CoreMotion
import UIKit
#if canImport(SplatKitCore)
import SplatKitCore
#endif

/// Turns the phone's orientation into a camera attitude.
///
/// Uses the arbitrary-heading, z-vertical reference frame: accelerometer plus gyroscope,
/// no magnetometer, so it is immune to magnetic interference and its heading is
/// arbitrary, the same trade as Android's game rotation vector. The matrix handed out
/// maps device axes to the reference frame (z up), remapped for the interface
/// orientation so landscape works.
final class MotionInput {
    private let manager = CMMotionManager()
    private let queue = OperationQueue()
    private let onAttitude: ([Float]) -> Void
    var interfaceOrientation: UIInterfaceOrientation = .portrait

    var isAvailable: Bool { manager.isDeviceMotionAvailable }

    init(onAttitude: @escaping ([Float]) -> Void) {
        self.onAttitude = onAttitude
        queue.name = "com.splatkit.motion"
        queue.maxConcurrentOperationCount = 1
        manager.deviceMotionUpdateInterval = 1.0 / 60.0
    }

    func start() {
        guard isAvailable, !manager.isDeviceMotionActive else { return }
        manager.startDeviceMotionUpdates(using: .xArbitraryCorrectedZVertical, to: queue) { [weak self] motion, _ in
            guard let self, let m = motion?.attitude.rotationMatrix else { return }
            self.onAttitude(self.remap(m))
        }
    }

    func stop() {
        manager.stopDeviceMotionUpdates()
    }

    /// CMRotationMatrix maps reference to device (m11 m12 m13 is its first row); the
    /// engine wants device to reference, its transpose. Then the device axes are turned
    /// so that "x right, y up on screen" holds in the current interface orientation.
    private func remap(_ m: CMRotationMatrix) -> [Float] {
        // Columns of the device-to-reference matrix: where each device axis points.
        let x = SIMD3<Float>(Float(m.m11), Float(m.m12), Float(m.m13))
        let y = SIMD3<Float>(Float(m.m21), Float(m.m22), Float(m.m23))
        let z = SIMD3<Float>(Float(m.m31), Float(m.m32), Float(m.m33))
        let (right, up): (SIMD3<Float>, SIMD3<Float>) = switch interfaceOrientation {
        case .landscapeRight: (y, -x)
        case .landscapeLeft: (-y, x)
        case .portraitUpsideDown: (-x, -y)
        default: (x, y)
        }
        return [right.x, up.x, z.x,
                right.y, up.y, z.y,
                right.z, up.z, z.z]
    }
}
