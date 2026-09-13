import Foundation

/// Command line switches, so a run can be scripted from `devicectl`:
/// `--world <file>` (a name in Documents or the bundle, or an absolute path),
/// `--tileset <file>` (a tileset.json), `--collider <file>`, `--residency <splats>`,
/// `--scale <f>`, `--sh <n>`, `--shdraw <n>`, `--budget <n>`, `--margin <deg>`,
/// `--linear`, `--gyro <0|1>`, `--pose x,y,z,yaw,pitch`, `--walk <m/s>`,
/// `--benchmark [seconds]`, `--capture <seconds>` (writes Documents/capture.png).
/// The default world is kitchen_500k.spz. ISS stays in Documents; use --world iss_10M.spz.
/// ISS orbits at a constant 45 m radius and 4 degrees/s; --radius, --speed and --start override it.
/// Its long X axis stays horizontal in screen space, without changing the orbit plane.
/// --orbit-horizontal 0 restores the previous vertical framing for benchmark comparisons.
/// Its starting pose is prepared during loading; motion starts only after the first
/// successful GPU frame. Loading stays covered while the renderer prepares that frame.
/// --orbit 0, --pose, --walk or --benchmark disable the automatic orbit and enable touch look.
/// No animated zoom. Gyro motion is opt-in with --gyro 1 on non-ISS worlds.
/// --metal-culling 1 opts into GPU-private scratch, footprint culling at 0.5 px,
/// and camera-depth sorting. Default 0 preserves the baseline; restart to change.
/// --min-pixel-radius <px> selects the experimental cutoff (default 0.5; try 1.0 or 1.2).
/// This is a source-footprint radius, not diameter, and has no effect without --metal-culling 1.
/// --depth-key-bits <16|32> selects linear camera-depth quantization and two radix
/// passes (16), or the unchanged four-pass ordering (32, default). Restart to change.
/// Quantization can change transparency ordering within a bin; no splats are removed.
/// --tile-raster 1 selects bounded hybrid 16x16 compute compositing, with T <= 0.0001.
/// Hardware fallback retains its existing 254/255 opacity coverage mask.
/// Dense tiles (>512 candidates) and tiles touched by large footprints (>16 tiles)
/// use hardware completion, never truncated lists. Scratch is capped at 128 MiB.
/// --keep-awake 1 disables idle screen locking only while this dev view is active.
/// --resource-monitor 1 logs process footprint, remaining process allowance, Metal
/// allocation and thermal state at 2 Hz, and pauses on memory/thermal pressure.
/// --memory-limit-mib <MiB> lowers its conservative 2800 MiB process-footprint guard.
/// --run-seconds <seconds> enables monitoring and pauses that long after the first world frame.
/// These dev-only guards stop future frames, not allocations or GPU work in flight.
/// Default 0 retains hardware rasterization. A GPU error stops submission until renderer recreation.
struct LaunchArgs {
    let values: [String: String]

    init(_ arguments: [String] = CommandLine.arguments) {
        var values: [String: String] = [:]
        var i = 1
        while i < arguments.count {
            let a = arguments[i]
            guard a.hasPrefix("--") else { i += 1; continue }
            let key = String(a.dropFirst(2))
            if i + 1 < arguments.count, !arguments[i + 1].hasPrefix("--") {
                values[key] = arguments[i + 1]
                i += 2
            } else {
                values[key] = ""
                i += 1
            }
        }
        self.values = values
    }

    func has(_ key: String) -> Bool { values[key] != nil }
    func string(_ key: String) -> String? { values[key].flatMap { $0.isEmpty ? nil : $0 } }
    func float(_ key: String) -> Float? { string(key).flatMap(Float.init) }
    func int(_ key: String) -> Int? { string(key).flatMap(Int.init) }
    func bool(_ key: String) -> Bool? {
        guard let v = values[key] else { return nil }
        return !(v == "0" || v == "false")
    }

    /// Resolves a world or collider argument: absolute paths as is, otherwise Documents
    /// first, then the bundle.
    static func resolve(_ name: String) -> URL? {
        if name.hasPrefix("/") { return URL(fileURLWithPath: name) }
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let inDocuments = documents.appendingPathComponent(name)
        if FileManager.default.fileExists(atPath: inDocuments.path) { return inDocuments }
        let url = URL(fileURLWithPath: name)
        return Bundle.main.url(forResource: url.deletingPathExtension().lastPathComponent,
                               withExtension: url.pathExtension)
    }
}
