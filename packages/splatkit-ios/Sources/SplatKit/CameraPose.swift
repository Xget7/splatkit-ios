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
