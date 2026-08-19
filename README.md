# FrameFlow

FrameFlow is a from-scratch Android 2× frame-interpolation prototype aimed at low-overhead Android devices such as Mali-G57 phones.

## Current implementation

- Android MediaProjection capture
- ImageReader/AHardwareBuffer input
- 8×8 block motion estimation
- ±3-pixel local search
- luminance block matching
- motion-confidence fallback
- half-way frame reprojection
- direct presentation to a Surface
- arm64-v8a native C++ implementation

This first build intentionally uses a **CPU reference implementation** for the motion estimator. It is designed as a correctness baseline before moving the exact algorithm to a Vulkan compute pipeline. That avoids the AHardwareBuffer external-memory/semaphore pitfalls that made the previous project fragile.

## Important limitation

This is not yet an LSFG-equivalent GPU implementation. On a Mali-G57, the CPU path is expected to be substantially slower than a proper Vulkan compute implementation, especially at high resolutions.

The intended next stage is:

1. Keep the capture/pacing layer unchanged.
2. Upload/import the two frames into Vulkan.
3. Estimate one motion vector per 8×8 block in compute.
4. Run a second compute pass for confidence/occlusion and reprojection.
5. Write the generated frame directly to a Vulkan presentation image.
6. Use fences/semaphores rather than `vkQueueWaitIdle`.

## Build

Open the folder in Android Studio with an installed Android SDK/NDK. The project uses compileSdk 35, minSdk 29 and arm64-v8a.

A local Android SDK/NDK is required; this environment does not contain the Android SDK, so an APK was not produced here.
