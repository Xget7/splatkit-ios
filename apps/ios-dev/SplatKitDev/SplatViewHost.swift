import SwiftUI
import UIKit
import Metal
import os

/// The one SplatMetalView of the app, shared with SwiftUI through an observable owner so
/// the HUD can read stats and push settings without recreating the view.
final class SplatSession: ObservableObject {
    let view = SplatMetalView()
    var orbit: OrbitPath?
    @Published var stats = SplatStats()
    @Published var status = "loading"
    @Published var walking = false
    @Published var motion = false
    @Published var gpu = ""
    @Published var scenePrepared = false
    @Published var loadingFailed = false
    private var timer: Timer?
    private var delegateBox: Delegate?
    private let monitorResources: Bool
    private let memoryLimitBytes: UInt64
    private let runSeconds: Double?
    private let resourceDevice: MTLDevice?
    private var memoryWarningObserver: NSObjectProtocol?
    private var firstLoadedTime: TimeInterval?
    private var peakFootprint: UInt64 = 0
    private let preparationStarted = ProcessInfo.processInfo.systemUptime
    private var resumed = false
    private(set) var stoppedReason: String?

    init() {
        let args = LaunchArgs()
        runSeconds = args.float("run-seconds").flatMap { $0.isFinite && $0 > 0 ? Double($0) : nil }
        monitorResources = (args.bool("resource-monitor") ?? false) || runSeconds != nil
        // A test guard, not a claim about the device's Jetsam limit or total free RAM.
        let limitMiB = min(max(args.int("memory-limit-mib") ?? 2800, 256), 2800)
        memoryLimitBytes = UInt64(limitMiB) * 1024 * 1024
        resourceDevice = monitorResources ? MTLCreateSystemDefaultDevice() : nil
        let delegate = Delegate(session: self)
        delegateBox = delegate
        view.delegate = delegate
        gpu = view.gpuDescription
        if monitorResources {
            memoryWarningObserver = NotificationCenter.default.addObserver(
                forName: UIApplication.didReceiveMemoryWarningNotification, object: nil, queue: .main
            ) { [weak self] _ in self?.stopRun("memory-warning") }
        }
    }

    deinit {
        if let memoryWarningObserver { NotificationCenter.default.removeObserver(memoryWarningObserver) }
    }

    func resume() {
        guard stoppedReason == nil else { return }
        resumed = true
        if scenePrepared { orbit?.begin() }
        view.resume()
        startPolling()
    }

    func pause() {
        resumed = false
        orbit?.end()
        view.pause()
        stopPolling()
    }

    func startPolling() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self else { return }
            self.stats = self.view.readStats()
            self.motion = self.view.isMotionEnabled
            if self.monitorResources { self.sampleResources() }
        }
    }

    func stopPolling() {
        timer?.invalidate()
        timer = nil
    }

    /// Dev-only observation at 2 Hz. Rendering and LOD selection remain entirely on GPU.
    /// The guard pauses future submissions; it cannot cancel GPU work already in flight
    /// or guarantee protection against a memory spike between samples.
    private func sampleResources() {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
        let result = withUnsafeMutablePointer(to: &info) { pointer in
            pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        guard result == KERN_SUCCESS else { stopRun("memory-sample-failed-\(result)"); return }
        let footprint = UInt64(info.phys_footprint)
        peakFootprint = max(peakFootprint, footprint)
        let available = os_proc_available_memory()
        let thermal = ProcessInfo.processInfo.thermalState
        let now = ProcessInfo.processInfo.systemUptime
        let elapsed = firstLoadedTime.map { now - $0 } ?? -1
        NSLog("SplatResources: elapsed=%.3f footprint=%llu peak=%llu available=%llu metal=%llu thermal=%ld loaded=%ld drawn=%ld paused=%d",
              elapsed, footprint, peakFootprint, UInt64(available), UInt64(resourceDevice?.currentAllocatedSize ?? 0),
              thermal.rawValue, stats.loadedSplatCount, stats.drawnSplatCount, stoppedReason == nil ? 0 : 1)
        if footprint >= memoryLimitBytes { stopRun("footprint-limit") }
        else if available < 256 * 1024 * 1024 { stopRun("available-memory-below-256MiB") }
        else if thermal == .serious || thermal == .critical { stopRun("thermal-\(thermal.rawValue)") }
        else if let runSeconds, elapsed >= runSeconds { stopRun("duration-complete") }
    }

    private func stopRun(_ reason: String) {
        guard stoppedReason == nil else { return }
        stoppedReason = reason
        resumed = false
        orbit?.end()
        view.pause()
        status = "paused: \(reason)"
        UIApplication.shared.isIdleTimerDisabled = false
        NSLog("SplatRunStopped: reason=%@", reason)
    }

    private final class Delegate: SplatViewDelegate {
        weak var session: SplatSession?
        init(session: SplatSession) { self.session = session }

        func splatView(_ view: SplatMetalView, worldReady splatCount: Int) {
            guard let session, session.stoppedReason == nil else { return }
            session.scenePrepared = false
            session.status = "\(splatCount) uploaded, preparing frame"
            NSLog("SplatPreparation: uploaded elapsed=%.3f", ProcessInfo.processInfo.systemUptime - session.preparationStarted)
        }

        func splatView(_ view: SplatMetalView, worldFrameReady splatCount: Int) {
            guard let session, session.stoppedReason == nil else { return }
            session.scenePrepared = true
            session.status = "\(splatCount) splats"
            session.firstLoadedTime = ProcessInfo.processInfo.systemUptime
            NSLog("SplatPreparation: first-frame-ready elapsed=%.3f", ProcessInfo.processInfo.systemUptime - session.preparationStarted)
            if session.resumed {
                session.orbit?.begin()
                NSLog("SplatPreparation: interaction-ready orbit=%d", session.orbit == nil ? 0 : 1)
            }
        }

        func splatView(_ view: SplatMetalView, worldFailed message: String) {
            session?.status = "world failed: \(message)"
            session?.loadingFailed = true
        }

        func splatViewColliderReady(_ view: SplatMetalView) {
            session?.walking = true
        }

        func splatView(_ view: SplatMetalView, colliderFailed message: String) {
            session?.status = "collider failed: \(message)"
        }
    }
}

struct SplatViewHost: UIViewRepresentable {
    let session: SplatSession

    func makeUIView(context: Context) -> SplatMetalView { session.view }
    func updateUIView(_ uiView: SplatMetalView, context: Context) {}
}
