# 0002. Vulkan directly, in C++

Status: accepted. Date: 2026-09-03.

## Context

Options considered: Vulkan via the NDK, wgpu in Rust (Brush), OpenGL ES 3.x, Filament.
Filament has no compute stage and its material system does not allow the covariance projection in the vertex stage.
wgpu adds a Rust toolchain to a React Native library, which most contributors will not touch.
OpenGL ES is frozen by Google and increasingly served through ANGLE on Vulkan.
Vulkan 1.1 reaches about 87 percent of active devices and is the official Android graphics API.

## Decision

Write the renderer against Vulkan 1.1 in C++17 with the NDK.
Use `vk-bootstrap` for instance, device and swapchain creation and `VulkanMemoryAllocator` for memory.
Shaders in GLSL compiled to SPIR-V at build time.
Vertex shader instanced quads, no mesh shaders, no ray tracing.

## Consequences

Boilerplate is paid once and owned.
Devices without Vulkan 1.1 (Android Go, low RAM) are unsupported until a GLES fallback exists.
`nvpro-samples/vk_gaussian_splatting` is the reference for the shader and data layout, not a dependency.
