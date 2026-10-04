# DLSS Frame Generation (DLSS-G)

Upscaling can drive frame generation with either AMD FSR3 (any vendor) or NVIDIA DLSS-G
(RTX 40 series and newer). Both run on the same D3D11-to-D3D12 proxy swap chain
(`DX12SwapChain`); which provider owns it is decided once at boot, in the
`D3D11CreateDeviceAndSwapChain` hook, and reported by `Upscaling::GetFrameGenMethod()`.

The DLSS-G path is Bottle Shaders' implementation (`961463d67`, 2026-10-04), which follows Open
Shaders' two-instance Streamline design. It replaced this fork's own port of the same design.

## Status

Untested on this fork's development machine (RTX 3090, which cannot run DLSS-G). On that
hardware the probe reports the feature unsupported and the FSR3 path is taken unchanged. The
upstream implementations were validated in game on RTX 40 hardware.

## Two Streamline instances

DLSS-G exists only for D3D12, while the rest of the plugin's Streamline use (DLSS upscaling,
Reflex, PCL) is D3D11. `Upscaling` therefore owns two `Streamline` objects:

| Instance                  | API   | Plugin directory                        | Features                   |
| ------------------------- | ----- | --------------------------------------- | -------------------------- |
| `Upscaling::streamline`   | D3D11 | `Data/Shaders/Upscaling/Streamline`     | DLSS, Reflex, PCL          |
| `Upscaling::streamlineDX12` | D3D12 | `Data/Shaders/Upscaling/StreamlineDX12` | DLSS-G, Reflex, PCL        |

Each loads its own `sl.interposer.dll` (Windows keeps them apart because the paths differ) and
calls `slInit` with its own `renderAPI`. The D3D12 instance also sets
`eUseFrameBasedResourceTagging`, since its inputs are tagged per frame token with
`slSetTagForFrame` on the present command list. `Streamline::instanceTag` distinguishes the two in
`CommunityShaders.log` (`[Streamline DX11]` and `[Streamline DX12]`).

The `StreamlineDX12` directory ships `sl.common`, `sl.interposer`, `sl.pcl`, `sl.reflex`,
`sl.dlss_g` and `nvngx_dlssg` as shipped by Bottle: Streamline 2.12.0 runtime (nvngx_dlssg 310.7),
while the D3D11 directory and the headers in `extern/Streamline-DX12` stay on 2.10.3. Each
instance loads only its own directory, so the two versions do not mix inside one interposer.

## Boot sequence

When frame generation is enabled and the proxy is going to be created:

1. `DX12SwapChain::CreateD3D12Device` creates the D3D12 device and queue (idempotent: the FSR
   path calls it again later and must keep this device).
2. `streamlineDX12.SetD3DDevice12`, `CheckFeatures`, `PostDevice`: DLSS-G availability is only
   reliable once a real device is bound. `PostDevice` binds `slDLSSGGetState`/`slDLSSGSetOptions`
   and reads `numFramesToGenerateMax` (1 on 2x-only hardware, 3 on 3x/4x).
3. If DLSS-G is available and the player has not ticked **Prefer AMD FSR Frame Generation**, the
   device is upgraded in place with `slUpgradeInterface`, the command queue is recreated on the
   upgraded device, and `CreateSwapChainDirect` creates a plain DXGI swap chain on the
   SL-upgraded factory with `max frames + 2` buffers (3 to 5) and without
   `FRAME_LATENCY_WAITABLE_OBJECT`. The swap chain is then upgraded too. `useDLSSG` is set.
4. Otherwise the FidelityFX frame-generation swap chain is created as before, on the clean
   (never upgraded) device. Upgrading unconditionally corrupted FSR3's state in Open Shaders'
   testing.

If NVIDIA Smooth Motion is enabled for the game's driver profile the proxy is skipped entirely;
both drivers' frame generation and this plugin's are known to crash together.

## Per frame

`Upscaling::CopySharedD3D12Resources` (post-processing) copies depth and motion vectors into the
shared D3D12 textures exactly as for FSR3. In `DX12SwapChain::Present`, the DLSS-G branch:

1. Emits `eSimulationEnd` and `eRenderSubmitStart` (`eSimulationStart` is emitted by
   `streamlineDX12.UpdateReflex` right after the Reflex sleep in the input hook).
2. Sets the frame constants (`CheckFrameConstants`, shared code with the D3D11 instance: same
   camera matrices, jitter, `depthInverted` from Reverse Z, `mvecScale` 1).
3. Tags depth and motion vectors with the render-resolution extent, the swap chain buffer
   (scene without interface) as HUD-less colour and the UI buffer as `UIColorAndAlpha`.
4. `ConfigureDLSSG(ShouldUseFrameGenerationThisFrame())` turns interpolation on or off for the
   frame and reads back `DLSSGState`. `lastDLSSGStatus` feeds `IsFrameGenerationActive()` and
   the settings panel warning.
5. After `ExecuteCommandLists`: `eRenderSubmitEnd`, `ePresentStart`, `Present`, `ePresentEnd`.

Streamline's present hook on the upgraded swap chain performs the interpolation; nothing else
in the plugin changes. `Upscaling::FrameLimiter` returns immediately under DLSS-G because the SL
pacer owns presentation.

## Reflex

DLSS-G requires Reflex at least in low latency mode while it generates frames. With DLSS-G the
D3D12 instance drives Reflex (`streamlineDX12.UpdateReflex`, called from the same input hook as
the D3D11 one) and forces low latency mode on whenever frame generation is prepared; the D3D11
instance stands down while the proxy swap chain is active, as before. The Reflex settings panel
targets whichever instance is active.

## Driver profile

The NVIDIA App's DLSS override writes DRS key `0x10308298` on the game's profile, which makes
`sl.dlss_g` report `eOk` and generate nothing. `Streamline::EnsureDriverProfileAllowsDLSSG`
(called from `Load()`, before any D3D device) resets it to the driver default through
`Util::NvApiDrs` (minimal `nvapi64` DRS binding, no SDK vendored). The App may re-assert the
key later, so this runs every boot and logs what it did.

## Settings

Upscaling panel, section **Frame Generation**. Persisted under `Upscaling`:

| Control                          | Key                     | Default |
| -------------------------------- | ----------------------- | ------- |
| Prefer AMD FSR Frame Generation  | `preferFSRFrameGen`     | off     |
| DLSS-G Frame Multiplier          | `dlssgFramesToGenerate` | 1 (2x)  |

`preferFSRFrameGen` is only shown when DLSS-G is available and needs a restart. The multiplier
is clamped to the hardware maximum both in `ConfigureDLSSG` and in
`GetFrameGenerationMultiplier`.

## Log lines to look for

```
[Streamline DX12] DLSS-G is available
[Streamline DX12] DLSS-G supports up to 4x frame generation
[Frame Generation] DLSS-G available, creating direct swap chain
[DX12SwapChain] Created direct swap chain for DLSS-G (3840x2160, 5 buffers)
[Streamline DX12] DLSS-G not generating frames this session: status=...
```

With Streamline log level 2 the presented-frame count is logged every 120 frames; `eOk` alone
does not prove interpolation, `numFramesActuallyPresented` does.
