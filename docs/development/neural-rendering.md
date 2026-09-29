# DLSS Neural Rendering

Upscaling owns a `NeuralRendering` pass that evaluates NGX Feature 18 (DLSS 5 neural rendering)
once per frame inside `Upscaling::Main_PostProcessing`, before DLSS/FSR upscaling and before frame
generation copies its inputs. It works on the render-resolution region of `kMAIN` and writes the
result back into it, so everything downstream is unchanged.

The pass is a port of Open Shaders PR #723 (`386bb211f`). It keeps that implementation's NGX
runtime, D3D11/D3D12 interop and colour contract. It leaves out the VR path and the developer
diagnostics suite (test sequences, traces, DDS captures, overlay).

## Status

Alpha, and **not verified in game on this fork**: the build and the shaders compile, nothing more
has been established. The reference implementation was validated by its authors on their hardware.

## Requirements

-   An NVIDIA RTX GPU, with DLSS selected as the upscaler at least once in the session: the pass
    binds the NGX parameter API of the `nvngx.dll` that the driver loads for DLSS.
-   `nvngx_dlssnr.dll` version **310.8.x**, placed by the player in
    `Data/Shaders/Upscaling/Streamline/`. **The mod does not ship this file and must never ship
    it.**

Only two builds had their output channel order checked in game by Open Shaders; their SHA-256 are
listed in `NR::kValidatedRuntimeSha256`. Any other 310.8 build is refused unless
**Allow Unvalidated Runtime Build** is ticked. That switch exists because the original model targets
recent GPUs and older RTX cards may need a converted build, whose hash differs. With it ticked the
colours of such a build are taken on trust.

## Settings

Upscaling panel, section **DLSS Neural Rendering**. All values persist under `Upscaling`:

| Control                         | Key                                            | Default |
| ------------------------------- | ---------------------------------------------- | ------- |
| Enable Neural Rendering         | `neuralRenderingEnabled`                       | off     |
| Allow Unvalidated Runtime Build | `neuralRenderingAllowUnvalidated`              | off     |
| Style                           | `neuralRenderingTuning.style`                  | 0       |
| Intensity                       | `neuralRenderingTuning.intensity`              | 1.0     |
| Local Tone Strength             | `neuralRenderingTuning.localToneStrength`      | 1.0     |
| Local Structure Strength        | `neuralRenderingTuning.localStructureStrength` | 1.0     |
| Skin Structure Strength         | `neuralRenderingTuning.skinStructureStrength`  | Auto    |
| Use Auto Mask                   | `neuralRenderingTuning.useAutoMask`            | on      |

The runtime latches the appearance controls when the feature is created, so committing an edit
recreates it. A failure pauses the pass and shows its reason in the panel and in
`CommunityShaders.log`; **Retry NR** rebuilds it on the next world frame. There is no automatic
retry.

## How a frame is processed

1. The render-resolution region of `kMAIN` is copied to `OriginalHDR`.
2. `ColorTransferCS.hlsl` (`Prepare`) builds a bounded, display-referred proxy of it: scene to
   linear, a hue-preserving soft knee, then sRGB. Feature 18 never sees unbounded scene values.
3. `GuidesCS.hlsl` extracts depth and motion into textures of exactly the render size. Depth is
   written as standard device depth (Reverse Z is undone here), motion is copied undilated.
4. Feature 18 is evaluated on a D3D12 queue created on the renderer's adapter. A shared fence
   orders the D3D11 writes, the D3D12 evaluation and the D3D11 reads.
5. `Composite` measures the tone difference between the proxy the model received and the one it
   returned, and applies it to the untouched original as a gain bounded to two stops. The model
   cannot change the frame's hue or alpha. When the two strength sliders differ, the difference is
   first split into a low and a high spatial band by a 5x5 edge-stopping filter.
6. The result is copied back into `kMAIN`.

The whole pass runs inside its own `ID3DDeviceContextState`, so none of its bindings leak into the
engine's state.

## Differences from Open Shaders

-   No scene white point: Open Shaders divides the input by a white point derived from its Post
    Processing adaptation. This fork tonemaps through Effects 11, which exposes no such value, so
    the scalar is 1. Very dark or very bright scenes reach the model less well normalized.
-   The guide encoder is a dedicated shader instead of `EncodeTexturesCS.hlsl`.
-   The shared fence lives in `NR::D3D12Interop`; this fork has no `SharedFence` type.
-   `NVSDK_NGX_Feature` 18 is cast from its number: the NGX headers in this tree stop at 16.
