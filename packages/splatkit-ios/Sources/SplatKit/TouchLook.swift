import UIKit
import UIKit.UIGestureRecognizerSubclass

/// Turns the camera while a finger drags the view: the only touch the SDK handles itself.
/// Walking comes from the host through `setWalkVelocity`, so the host's own controls, on
/// its own views, keep every other touch.
///
/// A gesture recognizer rather than the view's touch handlers: a host such as SwiftUI runs
/// its gestures over the whole hierarchy, and those take raw touches from the view.
final class TouchLook: UIGestureRecognizer, UIGestureRecognizerDelegate {
    /// Radians per point dragged.
    var sensitivity: Float = 0.004

    private let onLook: (Float, Float) -> Void
    private var touch: UITouch?

    init(view: UIView, onLook: @escaping (Float, Float) -> Void) {
        self.onLook = onLook
        super.init(target: nil, action: nil)
        cancelsTouchesInView = false
        delaysTouchesEnded = false
        delegate = self
        view.addGestureRecognizer(self)
    }

    // The host's controls recognize alongside this one.
    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer,
                           shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool {
        true
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        guard touch == nil, let first = touches.first else { return }
        touch = first
        state = .began
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent) {
        guard let view, let touch, touches.contains(touch) else { return }
        let p = touch.location(in: view)
        let q = touch.previousLocation(in: view)
        onLook(-Float(p.x - q.x) * sensitivity, -Float(p.y - q.y) * sensitivity)
        state = .changed
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent) {
        lift(touches)
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent) {
        lift(touches)
    }

    override func reset() {
        super.reset()
        touch = nil
    }

    /// Lets go of the finger, when the view leaves the screen or looking is turned off.
    func letGo() {
        touch = nil
        if state == .began || state == .changed { state = .ended }
    }

    private func lift(_ touches: Set<UITouch>) {
        guard let touch, touches.contains(touch) else { return }
        self.touch = nil
        state = (state == .began || state == .changed) ? .ended : .failed
    }
}
