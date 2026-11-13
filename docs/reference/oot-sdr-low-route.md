# Experimental OOT SDR MetalFX route

This branch preserves a working research candidate for contributors. It is **opt-in**, is not the default installation, and does not establish a performance improvement. HDR is not part of this work. Use a separate build and a copy of your own save when testing it.

## Behavior

The eligible world input stays at 1536x864 through MetalFX frame interpolation. Two MetalFX Linear spatial scalers produce the final native and generated images. Actual game UI draws are replayed over both final images from immutable resources, preserving their shaders, blend state and RGB masks. Alpha remains one. A terminal quad encodes linear RGB to SDR sRGB once. This avoids interpolating the already-upscaled 2048x1152 world, but **keeps the game's original upscale and HUD for safe fallback**. It does not remove that original GPU work.

An exact shader/bounds table qualifies a limited UI family for the audited `bc8830d4ba5a` shader chain. Its source SHA fields record provenance; they are not an automatic runtime shader-hash check. Requalify the table before using a different chain. Unsupported draws, views, resource copies, geometry paths or output shapes retain the original route. The branch currently recognizes the observed SDR RGBA8 base / sRGB view pairing and its actual attached format; it does not accept arbitrary view reinterpretations. Pool tables and packet resources remain leased until their real GPU readers finish. Warmup is required before activation.

## Build and activation

Build the host through the repository's usual macOS procedure for your own OOT build (`make -C dm/host GAME=0400486024DEC000`). The integrated candidate was built with the existing equivalent host link command on an M4 Pro, macOS27.3/SDK27.2; a clean build from this checkpoint is not separately claimed.

Compile the authored copy kernels separately:

```sh
xcrun -sdk macosx metal -std=metal4.0 -c dm/gfx/nvn/ui_snapshot_copy.metal -o /tmp/oot-ui-snapshot.air
xcrun -sdk macosx metallib /tmp/oot-ui-snapshot.air -o /tmp/oot-ui-snapshot.metallib
```

On the existing OOT MetalFX/FI launch configuration, supply:

```sh
export DM_OOT_HDR=0
export DM_OOT_HDR_OUTPUT_ALIAS=0
export DM_OOT_INTERPOLATION_PREVIEW=3
export DM_OOT_FRAMEGEN_ASYNC=1
export DM_LOW_ROUTE_ARM=/tmp/oot-low-route-arm
export DM_UI_SNAPSHOT_LIBRARY=/tmp/oot-ui-snapshot.metallib
export DM_LOW_ROUTE_REPORT=/tmp/oot-low-route-report.json
```

The ARM file must be absent at launch. After pool/pipeline warmup in the actual scene, creating it enables the candidate. **Activation is latched: deleting the file does not disable it. Restart the process to return to the old route.** `DM_LOW_ROUTE_SHADOW=1` still encodes the candidate; it is not a zero-cost OFF arm. The experimental route also requires the existing compatible MetalFX final-output setup and exact output/crop contract; the variables above are not a complete standalone game launcher.

Automated tests use `DM_AUDIO=1 DM_AUDIO_MUTE=1`, copied saves, and one guard attached to the real game PID. This branch supplies neither copyrighted game data nor a shader cache, NSO, save or prebuilt executable.

## Verification actually performed

The terminal transfer fixture (`dm/gfx/tests/t_sdr_transfer.m`) compares hardware sRGB attachment encoding against the exact piecewise shader formula. On M4 Pro it passed 32,768 pixels /98,304 RGB channels: maximum difference1LSB, no difference greater than1LSB, alpha and GPU errors zero. This is a transfer check, not a whole-game image-equivalence test. Reproduce it with `xcrun clang -O1 -fobjc-arc dm/gfx/tests/t_sdr_transfer.m -framework Foundation -framework Metal -o /tmp/oot-sdr-transfer` followed by `/tmp/oot-sdr-transfer`.

The integrated private build with these sources completed a silent139.83s Hyrule Castle Town run, including opening the inventory and returning to the world. There were2,972 packets taken and released,5,944 UI renders, zero UI failures, zero runtime private-table allocations and zero table-pool misses. Screenshots of the active world, inventory and recovered world were examined. Original executable, NSO and protected saves stayed unchanged.

The retained110-134s window showed101.04 actual displayed images/s, not120. It contained1,214 native and1,212 generated presentations, no abandoned drawable, maximum gap25ms,1,153 low-generation batches and61 legacy generations (5.02% fallback). Native GPU command-buffer envelope was18.087ms median /19.509ms p95, GPU1578MHz and thermal pressure0. These envelopes are not exclusive busy times. A previous candidate measured99.28images/s in another run; differing scene time/NPC state mean this is **not a causal A/B speedup**.

## Remaining work

- The next unsupported pair observed is VS`b05272e4699e2955` /FS`bbec88c9a94348a3`. Other UI scenes, dialogue, cutscenes and transitions remain unqualified. Preserve fallback.
- The low and legacy routes can differ in sharpness. The single-stage spatial reconstruction is softer; improved image quality is not established.
- Object motion is not yet supplied to FI. Existing animated-character/translucent-overlay interpolation artifacts remain open.
- Stable120Hz delivery and GPU headroom in heavy scenes are not proved. Prioritize the native rendering cost rather than repeatedly extending UI coverage as a proxy for speed.
- No broad multi-game regression or long-session certification was performed for this experimental branch. The normal fork remains on its previously tested commit.

The host-only Model feedback investigation is separate:32 shaders contain uniform texture-feedback operations added by the port. No feedback was removed in this branch. Any movement of those operations must preserve actual input versions and account for extra materialization caused by marking invisible draws.
