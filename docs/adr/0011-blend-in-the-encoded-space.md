# 0011. Blend in the encoded colour space

Status: accepted. Date: 2026-09-04.

## Context

The swapchain asked for an sRGB format, the shader converted colours to linear light and the blender mixed splats in linear light: physically right for light, and 40% of the frame on Adreno 640 (house, 2M splats, full resolution: 32.2 ms against 19.6 with a UNORM attachment of the same size; a 16 bit attachment gave the same 19.5, so the cost is the sRGB conversion per blended fragment, not the bytes per pixel).
The reference 3DGS rasterizer, and therefore the training that produced every splat file, blends the colour values as stored, with no linearisation; the trained colours only reproduce the photos under that blending.

## Decision

The swapchain and the offscreen target are UNORM and the shader writes the stored colours; splats blend in the encoded space, as they were trained.
`linearBlending` on the view asks for an sRGB swapchain and the old path, for hosts that want the richer contrast and can pay for it.

## Consequences

House 2M: 19.4 ms at full resolution, 13.6 ms (60 fps) at render scale 0.7. Kitchen 500k: 14.0 ms at full resolution.
Below render scale 0.7 the frame no longer gets cheaper (13.2 at 0.5): the floor is now per splat work, which is where the next optimisation has to look.
The image is lighter and lower in contrast than before, because the earlier one was wrong in the direction of more contrast.
