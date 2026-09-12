import Foundation
import QuartzCore
#if canImport(SplatKitCore)
import SplatKitCore
#endif

/// Owns the native engine and drives it from a dedicated thread, one frame per display
/// link tick, like the Android render thread.
///
/// The main thread never touches the engine. Everything is posted here; the calls that
/// must complete before the layer goes away block the caller until done. Decoding runs
/// on a loader queue so frames keep flowing; the engine uploads on its next frame.
final class RenderThread {
    private let thread: Thread
    private let ready = DispatchSemaphore(value: 0)
    private var runLoop: CFRunLoop!
    private var displayLink: CADisplayLink?
    private let loader = DispatchQueue(label: "com.splatkit.loader", qos: .userInitiated)
    // Created on the render thread; read by the any-thread getters (stats, camera pose).
    private(set) var engine: SKSplatEngine?
    private var rendering = false

    /// False when Metal could not be brought up; every call is then a no-op.
    var isAvailable: Bool { engine != nil }

    /// Loading outcomes, delivered on the main thread.
    var onEvent: ((SKSplatEvent, String, UInt32) -> Void)?

    init() {
        let started = DispatchSemaphore(value: 0)
        var engine: SKSplatEngine?
        var loop: CFRunLoop!
        thread = Thread {
            loop = CFRunLoopGetCurrent()
            // Everything that must live on the render thread is created there.
            engine = SKSplatEngine.create()
            started.signal()
            // The loop stays alive on a timer that never fires; work arrives as blocks.
            let keepAlive = Timer(timeInterval: .greatestFiniteMagnitude, repeats: true) { _ in }
            RunLoop.current.add(keepAlive, forMode: .default)
            RunLoop.current.run()
        }
        thread.name = "SplatKitRender"
        thread.qualityOfService = .userInteractive
        thread.start()
        started.wait()
        runLoop = loop
        self.engine = engine
        engine?.eventHandler = { [weak self] event, message, count in
            DispatchQueue.main.async { self?.onEvent?(event, message, count) }
        }
    }

    // Posting.

    /// Runs `block` on the render thread, later.
    func post(_ block: @escaping () -> Void) {
        CFRunLoopPerformBlock(runLoop, CFRunLoopMode.defaultMode.rawValue, block)
        CFRunLoopWakeUp(runLoop)
    }

    /// Runs `block` on the render thread and waits for it.
    func sync(_ block: @escaping () -> Void) {
        if Thread.current == thread {
            block()
            return
        }
        let done = DispatchSemaphore(value: 0)
        post {
            block()
            done.signal()
        }
        done.wait()
    }

    // Layer and lifecycle.

    /// Blocks: the engine draws on the layer as soon as this returns.
    func layerAttached(_ layer: CAMetalLayer, size: CGSize) {
        sync { [self] in
            engine?.setLayer(layer)
            engine?.setDrawableSize(size)
        }
    }

    func layerResized(_ size: CGSize) {
        post { [self] in engine?.setDrawableSize(size) }
    }

    /// Blocks: after this returns the layer may be released.
    func layerDetached() {
        sync { [self] in engine?.setLayer(nil) }
    }

    func resume() {
        post { [self] in
            guard !rendering else { return }
            rendering = true
            let link = CADisplayLink(target: self, selector: #selector(frame(_:)))
            link.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: 120, preferred: 120)
            link.add(to: RunLoop.current, forMode: .default)
            displayLink = link
        }
    }

    func pause() {
        post { [self] in
            rendering = false
            displayLink?.invalidate()
            displayLink = nil
        }
    }

    func release() {
        sync { [self] in
            rendering = false
            displayLink?.invalidate()
            displayLink = nil
            engine?.setLayer(nil)
            engine = nil
            CFRunLoopStop(CFRunLoopGetCurrent())
        }
    }

    @objc private func frame(_ link: CADisplayLink) {
        guard rendering else { return }
        engine?.render(Int64(link.timestamp * 1_000_000_000))
    }

    // Loading, on the loader queue.

    func loadWorldFile(_ path: String) {
        loader.async { [self] in engine?.loadWorldFile(path) }
    }

    func loadTiledWorldFile(_ path: String) {
        loader.async { [self] in engine?.loadTiledWorldFile(path) }
    }

    func loadColliderFile(_ path: String) {
        loader.async { [self] in engine?.loadColliderFile(path) }
    }

    // Camera and input.

    func look(_ deltaYaw: Float, _ deltaPitch: Float) {
        post { [self] in engine?.look(withDeltaYaw: deltaYaw, deltaPitch: deltaPitch) }
    }

    func walk(_ forward: Float, _ right: Float) {
        post { [self] in engine?.walkForward(forward, right: right) }
    }

    func setVelocity(_ forward: Float, _ right: Float) {
        post { [self] in engine?.setVelocityForward(forward, right: right) }
    }

    func setAttitude(_ rowMajor: [Float]) {
        post { [self] in rowMajor.withUnsafeBufferPointer { engine?.setAttitude($0.baseAddress!) } }
    }

    func setMotionEnabled(_ enabled: Bool) {
        post { [self] in engine?.setMotionEnabled(enabled) }
    }

    func lookAt(from position: SKVec3, target: SKVec3, up: SKVec3) {
        post { [self] in engine?.lookAt(from: position, target: target, up: up) }
    }

    func setCameraPose(_ pose: SKCameraPose) {
        post { [self] in engine?.cameraPose = pose }
    }

    func cameraPose() -> SKCameraPose? { engine?.cameraPose }

    // Settings.

    func setRenderScale(_ scale: Float) { post { [self] in engine?.setRenderScale(scale) } }
    func setCullMargin(_ degrees: Float) { post { [self] in engine?.setCullMargin(degrees) } }
    func setLinearBlending(_ linear: Bool) { post { [self] in engine?.setLinearBlending(linear) } }
    func setSplatBudget(_ budget: Int) { post { [self] in engine?.setSplatBudget(Int32(budget)) } }
    func setResidencyBudget(_ splats: Int) { post { [self] in engine?.setResidencyBudget(Int32(splats)) } }
    // This setting controls decoding as well as GPU upload, so apply it before a following
    // load can begin on the loader queue.
    func setMaxShDegree(_ degree: Int) { sync { [self] in engine?.setMaxShDegree(Int32(degree)) } }
    func setShDegree(_ degree: Int) { post { [self] in engine?.setShDegree(Int32(degree)) } }
    func startBenchmark(_ seconds: Float) { post { [self] in engine?.startBenchmark(seconds) } }

    var gpuDescription: String { engine?.gpuDescription ?? "" }
    func stats() -> SKSplatStats? { engine?.stats }
}

extension RenderThread {
    func captureFrame(_ path: String, completion: @escaping (Bool) -> Void) {
        post { [self] in
            guard let engine else { return completion(false) }
            engine.captureFrame(toFile: path, completion: completion)
        }
    }
}
