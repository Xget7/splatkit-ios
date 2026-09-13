import SwiftUI

/// A thumb stick that reports a direction in [-1, 1] on each axis while held.
struct Joystick: View {
    let onChange: (_ forward: Float, _ right: Float) -> Void
    @State private var offset = CGSize.zero
    private let radius: CGFloat = 60

    var body: some View {
        ZStack {
            Circle().fill(Color.white.opacity(0.12)).frame(width: radius * 2, height: radius * 2)
            Circle().fill(Color.white.opacity(0.5)).frame(width: 44, height: 44).offset(offset)
        }
        .frame(width: radius * 2, height: radius * 2)
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { g in
                    var d = g.translation
                    let len = hypot(d.width, d.height)
                    if len > radius { d = CGSize(width: d.width / len * radius, height: d.height / len * radius) }
                    offset = d
                    onChange(Float(-d.height / radius), Float(d.width / radius))
                }
                .onEnded { _ in
                    offset = .zero
                    onChange(0, 0)
                }
        )
    }
}
