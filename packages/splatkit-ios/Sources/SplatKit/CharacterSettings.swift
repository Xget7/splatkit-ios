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
