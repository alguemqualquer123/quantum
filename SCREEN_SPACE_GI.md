# Screen-space GI fallback

Quantum's forward renderer now exposes a documented fallback for scenes that need a
screen-space ambient contribution without a full clustered GI volume: enable `ssaoEnabled`
and keep the scene environment/reflection probes enabled. The SSAO pass reconstructs view-space
positions from the packed depth pre-pass, evaluates a rotated hemisphere and feeds the blurred
occlusion into the PBR lighting path. The environment/probe diffuse term supplies the indirect
colour, while SSAO supplies the short-range contact/bounce attenuation.

This is intentionally a bounded alternative, not a claim of full SSGI. It cannot transport
coloured light around corners or illuminate pixels that are absent from the camera depth. A
future true SSGI pass can replace the AO term without changing scene files because the fallback
uses the existing `ssaoRadius`, `ssaoBias` and `ssaoIntensity` controls.

For deterministic comparisons, capture the real presented frame with:

```text
quantum-engine --project <project> --window 1280x720 --screenshot <frame.png> --capture-frame 120
```
