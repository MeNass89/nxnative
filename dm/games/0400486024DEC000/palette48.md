# Experimental palette48 source rewrite

This tool rewrites **five exact, SHA-allowlisted translated vertex sources**. It is a reproducible source experiment, not a general shader optimizer. It includes no game shader bodies, shader dumps, game binaries, or saves. Use your own local source files.

For each 48-byte bone record, a bounds-checked fast path loads three `float4` values. Partial records and wrapped addresses retain the original twelve protected scalar expressions, extracted from your input. All three original entry points remain. The tool checks unchanged floating-point/FMA text, verifies an exact inverse, applies `always_inline`, and requires the output SHA to match the corresponding compiled experimental source.

## Usage

Python 3.9 or newer; standard library only:

```bash
python3 dm/tools/oot_palette48.py original.metal candidate.metal --report proof.json
```

The input is selected by its bytes, not its filename. Unknown or modified inputs fail before transformation. Even a newline change is rejected. `candidate.metal` and `proof.json` must be new, separate paths in existing directories. Existing files and symlinks are never overwritten; the input is not edited and no backup is needed. The optional report contains hashes and verification results, not shader source.

The five supported keys are `e3d81ddfed3e0ec8_b`, `fbc55ea8f49e4772_b`, `566c7e0c6e8c5dc5_b`, `090e3ef632e3222a_b`, and `371fbb2aa4022b42_b`. Their exact original/output SHA pairs are embedded in the tool. There is no override or fallback for other versions.

The tool does not compile, install, upload, or collect anything. Use the fork's existing compilation/integration path; the source contract was checked with `-O1 -fpreserve-invariance -fwrapv`.

## Validation boundary

A separate GPU fixture on Apple M4 Pro passed 12,800 cases: 153,600 palette words per implementation, bit exact, including signed zero, denormals, infinities, NaN payloads, partial records, and 32-bit wrap. This qualifies the **twelve palette values**. It does not qualify all fifteen vertex outputs, production integration, or a frame-rate improvement. FPS benefit remains unknown. Keep this experimental until those checks are complete.

A private combined-library smoke on the M4 Pro also rendered Kokiri Forest and Castle Town with the existing SDR MetalFX/interpolation route and no reported GPU error. This is a short functional check, not full-frame equivalence, sustained stability, or a measured benefit from the palette change. No default cache, host or application is changed by this tool.
