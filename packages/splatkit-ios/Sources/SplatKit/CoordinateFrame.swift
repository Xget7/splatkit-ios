#if canImport(SplatKitCore)
import SplatKitCore
#endif

/// Where a file's +X, +Y and +Z point, as three letters from r, l, u, d, b and f: `rub` is glTF
/// and OpenGL, `rdf` OpenCV, COLMAP and World Labs Marble, `rfu` Blender. Any of the 48
/// conventions, right or left handed.
public struct CoordinateFrame: Hashable, Sendable {
    public static let rub = CoordinateFrame(canonical: "rub")
    public static let rdf = CoordinateFrame(canonical: "rdf")

    /// Lower case.
    public let letters: String

    /// Nil unless `letters`, in either case, name three distinct axes.
    public init?(_ letters: String) {
        guard let canonical = SKSplatEngine.canonicalSourceFrame(letters) else { return nil }
        self.letters = canonical
    }

    private init(canonical letters: String) {
        self.letters = letters
    }
}
