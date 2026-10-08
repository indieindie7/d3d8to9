# d3d8 layer optimisation: readback stalls and redundant state (2026-10-08)

Scope: the d3d8to9 fork (`gi-cascades`) used by Advent Rising and Unreal II. Follows the
optimisation section of `codes/games/research_notes/Advent graphics survey/survey.md` (items
"Remove CPU/GPU sync points" and the redundant-state idea). Offline work only: everything here
compiles (Release|Win32, v145), nothing has been run in a game yet.

## TL;DR

- **No per-frame GPU->CPU readback exists in the layer.** Every `GetRenderTargetData` /
  read-only `LockRect` of video memory runs on an event (screenshot, sketch, texedit click) or
  in a debug/test mode, at most a few times per session. The survey's suspects are all GPU-only
  in normal play: GI reads depth through an INTZ *texture* in the shader, PCSS copies its map
  with `StretchRect` (GPU to GPU), blood/runs/streaks only upload. So there was nothing to
  convert to an N-2 ring; adding one would only add latency and code.
- **New perf fields** (every 30 s in `U2Shaders.log`, next to the existing fps line) measure the
  layer's own CPU time, readback time and the game's state traffic, so this conclusion and any
  later change can be checked in game.
- **`redund=1` was not added.** See "Redundant state" below: the device is not a pure device, so
  the Direct3D 9 runtime already drops repeated states, and a safe layer-side filter needs more
  than "dirty all" around our passes (our per-draw hooks change state on any draw). The new
  counters tell us whether it could ever pay; the design is written down for that case.

## A. Readback table

"Stalls?" = does the CPU wait for the GPU. Frequency in normal play (no debug settings).

| Where (file:line) | What | How often | Needs the data for | Stalls? |
|---|---|---|---|---|
| u2shaders.hpp:4616 `SkSave` | ImGui sketch rendered to a target, `GetRenderTargetData` | user saves a sketch | PNG file | yes, once per save; fine |
| u2shaders.hpp:4048 `SkReadBack` | frame copy (`StretchRect` at grab) read at the **next** Present | user grabs a frame for the sketch | sketch background | short (1 frame late already) |
| u2shaders.hpp:8568 `ShotP` | back buffer (resolved if msaa) | `shotp` / AdventNative "Capture" / pilot `shotp` step | ShotP#####.bmp | yes, screenshot only; fine |
| u2shaders.hpp:4879 `MaskSave` | character mask target | with ShotP when shotmask=1 | _mask.bmp | yes, screenshot only; fine |
| texedit.hpp:467 `PickRead` | 1x1 copy of the pick target, read one frame after `PickResolve` | texedit click | which draw is under the cursor | at most a short wait (1 frame late, 4 bytes); fine |
| u2shaders.hpp:6856 `MsaaSelfTest` | 64x64 test target | once per session with msaa=N | log line: does the card multisample | once; fine |
| u2shaders.hpp:4939 `RtLeave` | 512x512 game render target | rtdump=N (testing) | RtDump##.bmp | debug only |
| u2shaders.hpp:1281 `DumpRaw` | PCSS maps | pcssdebug>2.5, once per size | raw dumps | debug only |
| u2shaders.hpp:1326 `CountShadowed` | PCSS silhouette map / snapshot | pcssprobe=N, first N draws | log counts | debug only |
| u2shaders.hpp:5917 `LogUnreadableDraw` | projector's shadow texture | log=1, once per size after frame 200 | .dds dump | debug only |
| u2shaders.hpp:5146 | `LockRect` READONLY on a cube map's faces | pcssdebug>5.5, once per setup | log alpha range | debug only (managed cube) |
| u2shaders.hpp:475 `Hash`, :5376 `TextureLevels`, :859 `GradeCopy` | `LockRect` READONLY on the game's textures | once per texture (cached) | texture hash / levels / graded copy | no: managed/system copies; default-pool targets fail the lock and are marked unreadable |
| d3d8to9_device.cpp:667 `GetFrontBuffer` | game's own call | whatever the game does (screenshots) | game | yes, game-initiated |
| d3d8to9_device.cpp:594 `CopyRects` (D3DX path) | game copies a video-memory surface into system memory | game-initiated | game | yes if the game does it |
| d3d8to9_device.cpp:1260 `GetInfo` | one `GetData(D3DGETDATA_FLUSH)` (no spin) | game-initiated; UE2 not seen calling it | game | flush only |

Not readbacks, checked because the survey named them:

- **GI depth** (`u2shaders.hpp` ~6601-6680): the game's depth surface is swapped for an INTZ
  depth *texture*; gi/ssao/sss sample it in their shaders. No CPU round trip.
- **PCSS** (`u2shaders.hpp:5089`): the silhouette map is copied per shadow with `StretchRect`
  into a render-target texture the projector samples. GPU to GPU.
- **blood / runs** (`blood.hpp:323`, `runs.hpp:217`): `LockRect(0)` on MANAGED textures =
  CPU writes into the system copy, uploaded on use. No wait on the GPU.
- **Quad vertex buffers**: gi/ssao/smaa quads are MANAGED and written once; the post quad is a
  DYNAMIC buffer locked with `D3DLOCK_DISCARD`. No waits.
- No occlusion or event queries anywhere in the layer; no `GetData` spins.

### What changed (A)

1. Every layer readback now goes through `U2PerfReadback()` (perf.hpp), a timed
   `GetRenderTargetData`; the game's reads through us (GetFrontBuffer, CopyRects into system
   memory, `LockRect` on a render-target/depth surface in `d3d8to9_surface.cpp`) are timed too.
2. No ring buffer / event queries were added: there is no per-frame readback to convert.
   If one is ever added (e.g. auto-exposure reading the frame's brightness on the CPU), the
   pattern is: 3 lockable SYSTEMMEM surfaces + 3 `D3DQUERYTYPE_EVENT` queries; at frame N
   `GetRenderTargetData` into slot N%3 is **not** usable (it blocks by itself), so copy GPU-side
   first (`StretchRect` into a small DEFAULT render target ring), `Issue(D3DISSUE_END)` on the
   slot's query, and at frame N+2 call `GetData(nullptr, 0, 0)` (no FLUSH); only when it returns
   S_OK do `GetRenderTargetData` + `LockRect` on that slot (now free), else keep the last value.
   Better still, keep such values on the GPU (sample a 1x1 texture in the shader).

## New perf fields

Every 30 s, after the existing fps line:

```
perf: 30 s, 1650 frames: avg 55.0 fps, 1% low 31.2 fps, worst 92 ms, 3 over 50 ms
perf: layer 1.84 ms/frame (eof 0.21, post 1.02, draws 0.61), readback 0.00 ms/frame (0 reads), game 0.00 (0)
perf: state calls/frame rs 812 (41% repeat), tss 655 (37%), samp 240 (52%), tex 390 (28%), 0.29 ms/frame
perf: device behaviour flags 00000040: not a pure device (the Direct3D 9 runtime drops repeated states itself)   (once)
```

- **layer** = CPU time inside the layer's own code, split into parts that add up (a nested part
  is taken out of its parent):
  - **eof**: the work at Present before the driver's Present (msaa resolve, texedit,
    `U2Shaders::OnPresent`; a post chain run from there counts as post, a shot as readback).
  - **post**: `RunPost` (bloom, gi, ssao, sss, smaa, lens) wherever it runs (first 2D draw or
    Present).
  - **draws**: the per-draw hooks (`U2Begin`, `U2After`, gloss/streak/mask/pick extra draws).
    Sampled on 1 draw in 8, scaled by 8, so it costs almost nothing.
  These are CPU submission times. GPU time isn't measured (that would need timestamp queries).
- **readback** = time blocked in the layer's `GetRenderTargetData` and how many there were;
  **game** = the same for the game's own reads through us. Expected 0 in play; ShotP shots show
  up here (a few ms each), which is the point: you can see them.
- **state calls** = the game's `SetRenderState` (rs), `SetTextureStageState` non-sampler (tss)
  and sampler kinds (samp: address, border, filters, mip bias/level, anisotropy),
  `SetTexture` (tex), per frame, and the share that repeat the value the game itself set last in
  that slot (counted as the game asked, before the aniso= and msaa rewrites; a state block Apply
  or a Reset forgets the slots; Sets while recording a block don't count as repeats).
  The ms/frame is the CPU time inside those wrappers (ours + runtime + driver), sampled 1 in 8.

Timing uses `__rdtsc`, converted against `QueryPerformanceCounter` over each 30 s window
(invariant TSC on any CPU these games run on now).

## B. Redundant state

### Counters (done)

See "state calls" above. Cost: one increment, one compare and a slot write per call; a time
stamp pair on 1 call in 8.

### Filter (`redund=1`): not added, and why

1. **The runtime already does it.** The game creates a non-pure device (the layer depends on
   `Get*` calls, which a pure device refuses, and the new one-time perf line prints the flags to
   confirm). For non-pure devices the Direct3D 9 runtime compares each `SetRenderState`,
   `SetTextureStageState`, `SetSamplerState` (and `SetTexture`) with its own copy and drops
   repeats before the driver (the debug runtime's "Ignoring redundant Set..." messages). So a
   layer filter only saves the call into d3d9 and the runtime's compare: tens of nanoseconds per
   repeat. At 1000 repeats a frame that is a few hundredths of a millisecond.
   The "state calls ... ms/frame" field is the ceiling: if the whole state traffic costs
   0.3 ms/frame, filtering the repeated part can save at most a fraction of that.
2. **"Dirty all" around our passes is not enough here.** Big passes (RunPost, gi/ssao/smaa,
   texedit pick, sketch, ShotP, msaa self-test) save and restore state and could be bracketed.
   But the per-draw hooks change device state on ordinary game draws: `replace=` swaps textures
   and `U2After` puts them back, `RelightBegin/End`, `PcssBegin`, `U2.Begin/End` (rule shaders,
   sampler 1-3 states, texture transforms), gloss/streak/mask/pick passes. They restore by
   re-setting saved values, and that is usually exact, but proving it means auditing ~40 hook
   sites in a 9300-line file, and one missed restore = a state the game set being silently
   dropped (wrong blending/filtering on some draw, hard to trace). A dirty-all on every hook
   that ran would invalidate the shadow on almost every draw, and the filter would catch nearly
   nothing.

### If the counters ever say it's worth it

Trigger: "state calls" above ~1 ms/frame with high repeat shares, on a pure device or after
measuring that our wrappers themselves are the cost. Safe design:

- Filter at the **D3D9 level, on final values** (after the aniso=/msaa rewrites), so the
  game-asked vs device-value question goes away: skip only when the device already holds that
  exact value.
- Keep the shadow honest without an audit: when `redund=1`, hook the proxy device's vtable slots
  for `SetRenderState`, `SetTextureStageState`, `SetSamplerState`, `SetTexture` (and
  `IDirect3DStateBlock9::Apply`), so *every* state change, the layer's and ImGui's included,
  updates the shadow; `Apply`, `Reset`, `BeginStateBlock..EndStateBlock` forget it. Only calls
  flagged as coming from the game's wrappers are filtered. Install only when `redund=1`, so the
  default path is untouched.
- Alternative without hooks: a generation counter bumped by a `U2DirtyAll()` call in every
  pass and in each per-draw hook *when it actually changed something* (each hook returning
  whether it touched state). Needs the audit above.

## Other findings (not changed)

- `CopyRects` with a multisampled source (msaa=N, game copying from the back buffer) creates and
  releases a full-screen render target on every call (`d3d8to9_device.cpp` ~550). If the game
  does that per frame, cache the resolve target (release it in `Reset`). The "game" readback
  counter doesn't cover it (GPU-only copy); watch for it with msaa on.
- The post chain's quad VB is locked with DISCARD for every quad (~10-20 a frame with the bloom
  chain). Cheap, but a NOOVERWRITE ring of 4-vertex slots would avoid driver buffer renaming.

## How to measure (in game, by the user)

Pilot: Advent heavy fight, `fps_fight.ps1` in the scratchpad (two 30 s perf windows, 3 shotp).

1. **Before**: the build from HEAD (`bin\Release\d3d8.dll`, rebuilt from the unchanged tree)
   only prints the fps line. Run the pilot, keep `U2Shaders.log`.
2. **After**: build this tree (the offline build is in `bin\OptTest\d3d8.dll`), run the same
   pilot. Note: the script trims log lines to 170 characters; the new lines fit (~110).
3. Read:
   - avg fps / 1% low should match "before" within noise (instrumentation is ~1 compare per
     state call; if 1% low drops, the counters are not as cheap as assumed: report it).
   - `readback` should be 0 except in windows with a `shotp` (a few ms per shot, counted).
     A non-zero value in a window without shots = a stall we haven't found: report it.
   - `game` reads: non-zero = Advent itself reads back the GPU (worth a look).
   - `layer` split: where our CPU time goes (post vs per-draw hooks vs eof). The biggest part is
     the next optimisation target (e.g. per-draw rule lookups in `U2Begin`).
   - `state calls`: if the ms/frame is well under 1 ms, a redundancy filter is not worth it.
4. Repeat once with gi/ssao/smaa off in `U2Shaders.ini` to see the post part move.

## Risks

- The counters run on every game state call and every draw (an increment + compare; time stamps
  on 1 in 8). Expected cost < 0.05 ms/frame; confirm with the fps line.
- `__rdtsc` on a CPU without invariant TSC would skew the ms values (not the counts). Not a
  concern on current hardware.
- The surface `LockRect` wrapper now calls `GetDesc` per lock (to tell render targets apart).
  Surface locks are rare in UE2 (textures are locked through the texture interface).
- Repeat counts are relative to the game's own last value, not to the device's: an upper bound
  for what a filter could drop, not exactly what it would drop.

## Results (2026-10-08, Advent heavy-fight pilot, 1080p, vsync 60)

| | before | after |
|---|---|---|
| the game's own readback (ReduceMouseLag: a 1x1 READONLY lock of the back buffer every frame) | 3.3-3.6 ms/frame | 0 (lagfix=1: dummy pixel + previous-frame event wait at Present, wait measured ~0) |
| layer end-of-frame work in a fight (blood pool sim + run sheets, every frame) | 5.07 ms/frame | 1.11 ms/frame (pools and runs step at 30 Hz) |
| game state calls (redundancy filter candidate) | 0.08 ms/frame | not filtered: not worth it |
| aniso=16 (game asks 4x) | | ~1-2% sharper distant floor (Laplacian 257 -> 261); kept on, cost negligible |

lagfix keeps mouse lag at about one frame (the CPU waits for the previous frame's GPU work at Present),
instead of a full-frame stall each frame. lagfix=0 restores the game's own lock.
