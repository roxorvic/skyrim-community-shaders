# Ground fog v3: a heavy fog layer on a baked height map (D194)

Goal: replace fog v2's three nested wind levels with one simple model, a thin heavy layer of fog lying on a baked height
map, drawn as a volume. Simpler, lighter and better looking, in the same budget: 1 ms GPU in total, nearly nothing on the
main thread. Fog-v2 (`fog-v2.md`) sections 4 (placement and timing) and 5 (look) carry over unless noted; its sections 1-3
are replaced.

Why: fog isn't air. Carrying it on an incompressible air sim leaked across ledges, needed a pressure solve, and every fix
added machinery (air-height tags, face walls, two fog layers, level matching, three levels with hand-offs). In a fog layer
fog only flows from a higher fog surface to a lower one: ledges, pooling and draining all follow from that one rule.

## Design

### 3.1 Ground (`GroundTiles`, replaces `GroundHeightMap`)

**Tiles:**

- **Size:** fixed world squares, 1,024 units across at 8 units per texel (128²).
- **Contents, two channels:** **height**, and **flags** (bit 0 _fog may form_, bit 1 _rock/log_, bit 2 _steep_, carries no
  fog).
- **Built from** the landscape plus `GroundMeshes`' triangles, with today's classification:
    - plants, clutter, effects, markers and critters are ignored;
    - rocks, logs, trees, stumps and mountains are rock/log;
    - everything else goes by facing: up within 300 u of the landscape is ground, except `Landscape\` meshes which count at
      any height; steep is flagged.
- **Solid hollows:** boats, ships, carts, wagons, furniture, coffins and troughs count as rock: fog forms inside a boat
  otherwise (Riverwood, 2026-11-11). Tents stay ground. Add a model family here with the spot that showed it.
- **Fires** (campfires, fire pits, fireplaces, braziers, forges, smelters, hearths, fire effects) clear the fog round them.
- **Overhangs (D195):** two levels per column: the top surface and, under an overhang, the floor below it (height and
  flags for each).

**Building:**

- On a **worker thread** under a time budget, nearest first and biased toward the direction of travel.
- The main thread only copies the statics' triangles (as `GroundMeshes` does now) and later uploads finished tiles.
- **Disk cache:** per worldspace, keyed by the load order (plugin list hash) and the tile's coordinates. Written once, when
  a tile is first built, never during normal play.

**Resident texture:**

- The tiles round the player, mapped into one toroidal texture (for example 1,024² at 8 u = ±4,096 u). Scrolling copies
  tiles; nothing is rasterised.
- Where a tile isn't ready, it's filled from the landscape (tier 2) at once, and replaced when the tile arrives.

**Far ground (agreed deviation, 2026-11-12):** the landscape's own depth (`LandscapeDepth`): during the depth pre-pass every
landscape pass (LOD landscape included) is drawn once more into a depth target of its own, the way Terrain Blending does
(hooks chained, no CS core change). The distant fog is one lookup per pixel: a layer on the surface where the scene's depth is
the landscape's (trees, rocks and buildings stand in front; water lies over its bed), reaching Distant Fog Reach (default
90%) of the drawn landscape's reach. Replaced a first try the same night that rasterized the LOD meshes into a height map
(its heights did not match the terrain drawn). Replaces the Terrain Shadows height map below: decoded on 2026-11-11 it is quantized in ~2,056 u steps, too coarse
for a 20 u layer. The loaded-landscape middle tier (below) was built late, the same day.

**Far ground (as first planned):**

- The Terrain Shadows height map for the worldspace (`features/Terrain Shadows - Heightmaps`). Its filename encodes the
  extents; reuse `TerrainShadows::HeightMapMetadata` and its loader. Load it ourselves if Terrain Shadows is off.
- **Modded worldspaces without one:** loaded-cell landscape only, so the far fog's range is shorter. A user-generated
  height map (xLODGen style, which Terrain Shadows already parses) is the fix.

### 3.2 Fog solver (`FogSolver`, replaces `WindSolver` for the fog)

**Grid:** one 512² toroidal grid round the player. **Fog Detail** picks the cell size at a fixed cost: 12 u (±43 m),
**16 u (±58 m, default)** or 24 u (±88 m). The bed for each cell is the tiles' ground, from 2×2 fine texels, keeping
steps sharp (the highest where a step crosses the cell).

**State:**

- `Depth` (R32F);
- `Flux` (RGBA16F: outflow east, west, north, south);
- `Velocity` (RG16F, derived from flux/depth, for confinement, wisps and the draw).

**One step per frame, three passes:**

1. **Inputs:**
    - **Sources:** fill toward Ground Thickness where _fog may form_, at a rate set by Ease In:
      `depth += SourceRate * max(GroundThickness - depth, 0) * dt`.
    - **Evaporation:** `depth -= EvapRate * max(depth - PoolDepth, 0) * dt`.
    - **Cuts:** remove depth.
    - **Pushes, weather wind and vorticity confinement:** add to the flux, using last step's velocity.
2. **Flux:** each outflow accelerates by the surface difference to that neighbour, `(ground + depth) here − there`, times
   the fog's reduced gravity.
    - **Rocks and logs (one-way):** flow _into_ a rock/log cell is allowed only if this cell's ground is above the rock's
      top. Fog may land on it from above; it drains off by the same rule.
    - **Ledges and spill:** nothing special, they follow from the rule.
    - Scale outflows so a cell never sends more than it holds. This is the standard pipe-model limit, and it keeps depth
      ≥ 0 and conserves fog.
3. **Depth and output:** depth += inflow − outflow. Derive the velocity, and write the column texture (3.4).

**Seeding the settled field:**

- **After a load, fast travel, teleport or reset**, depth starts from the settled field instead of zero:
  `min(PoolDepth, GroundThickness + hollowness · k)`. Hollowness is the ground's depth below its blurred surroundings,
  computed on the GPU from the ground in one pass.
- **During play:** nothing is seeded, so fog builds and pools gradually.

**Weather and time:** for now, scale Ground Thickness and Pool Depth by one _fogginess_ value (0–1, fixed until the
placement step). Fog arriving raises both slowly, so fog forms on slopes, drains, and fills low spots first. Fog leaving
lowers both, so pools shrink from the top.

**Removed:** levels, parent/child, absorb, edge bands, wake/idle, the far update rate, air height, the wall mask, face walls
and the second fog layer.

### 3.3 Movers (push and cut)

- **Gathering:** the existing `AirSources` (bones: feet, calves, hips, whole bodies for creatures), extended, under one
  ranked list (distance × speed, caps per kind, coarsening skeleton → capsule → none), as in fog-v2 section 3.
- **The primitive:** a segment (from, to), a radius, a z range (bottom, top), a push velocity and a cut amount.
- **Mid range:** one capsule per actor (or per limb group) **splatted** into a force texture at the sim's resolution. The
  splat stays area-bound, with integer atomics as now, but needs only **force x, force y and cut**, not seven channels.
  Pushes go into the flux in pass 1, so fog piles in front, parts, and leaves a wake.
- **Close range (Contact Radius, default 18 m, range 15–20):** every bone, weapon and projectile capsule is also drawn each
  frame into a **contact map**: a 2D texture round the player at about 6 u per texel (512² ≈ ±22 m), storing the cut's
  bottom and top height. The draw reads one texel per march step and clears fog inside that height range, smoothly. That
  gives exact leg- and blade-shaped holes in 3D without a finer sim. The same capsules also push the sim.
- **Mover order** (after the foundation works):
    1. bones and torso motion;
    2. melee swings (swept segment from last frame: push along the swing, cut along the blade);
    3. projectiles and spells (path segments, beams as continuous lines, a burst on impact);
    4. explosions (radial burst plus cut);
    5. shouts (a cone push);
    6. physics bodies above a speed threshold (ragdolls by their bones).
       Projectiles and explosions come from hooks or events, not reference scans.

### 3.4 Column texture

Written by the solver's output pass for the sim area: **depth**, **velocity**, and **churn** (speed plus |curl|, for the
lift). The ground comes from the resident ground texture at 8 u, so ledges stay sharp in the draw even though the sim is at
16 u. A **max mip** of `ground + depth` lets the march bound each ray cheaply.

### 3.5 Draw (rework of `GroundFogCS.hlsl`)

**Keep:** the half-resolution march, the depth-aware composite, the sun HG plus ambient lighting, and self-shading from
the distance to the fog top.

**The slab:** per march sample, ground (fine) + depth (column) give the fog's bottom and top.

- **Density:** `Density × profile(height in column) × noise3D`.
- **Profile:** dense at the bottom, breaking into clumps toward the top.
- **Across a step:** bilinear only between columns whose ground is within a step of the sample's ground, so there's no fog
  ramp hung across a ledge. That's one comparison per tap.

**Ray bounds:** from the max mip, replacing the 10 band probes. Start at 16–24 steps and keep the existing geometric step
growth.

**Far fog:** beyond the sim grid, the same slab with the far height map and the settled-field depth (no sim). Fade from the
sim's depth to the settled depth over a band at the grid's edge. They match because the sim starts from the settled field.

**Fluff:**

- **3D noise:** 64³, R8, built once at startup like today's 2D texture. Carried by the column's velocity (two-phase flow
  mapping on xy) and drifting slowly upward.
- **Puff by depth:** clumping and top billows scale with depth, so 40 u pools billow and 20 u flats stay calmer.
- **Lift:** churn raises the top locally, and it settles back.
- **Wisps:** a few above the top around movers and in deep pools.

**Contact cuts:** within Contact Radius, density is zero (soft edge) inside the contact map's height range.

**Removed:** the two ground maps' level logic, the overhang floor, the three-level fog/wind sampling, level matching, the
resting-pattern hash per sample, and the 2D wisps and billows.

### 3.6 Settings and debug

**Settings:**

- Enabled.
- Density.
- Brightness / Albedo.
- Ground Thickness (20).
- Pool Depth (40).
- Ease In (s).
- Fog Detail (12 / 16 / 24).
- Contact Radius (15–20 m).
- Stir (push strength).
- Debug View.

**Gone:** Pressure Iterations, Sim Levels, Fine Radius, Far Update Rate, Wind Damping, Refill, and Fog Thickness (now
Ground Thickness).

**One debug view:**

- **Grey:** fog depth on the ground.
- **Green:** fog may form.
- **Red:** rock/log.
- **Yellow:** steep.
- **Blue:** unknown, or the far tier.
- **Overlay:** the contact map.

**Always switch the debug view off after a test, and say so.**

### 3.7 Code: keep, rewrite, delete

| Keep                                                                                                                                                                                                                  | Rewrite                                                                                                                                                                             | Delete                                                                                                                                                       |
| --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `GroundMeshes` (triangle reading and classification), `LandscapeCache`, `Field`, `AirSources` (extend), the noise builder (make it 3D), the half-res march and composite plumbing, the profiler and GPU-span wrapping | `GroundHeightMap` → `GroundTiles`; `SplatCS` (three channels plus the contact map); `GroundFogCS` (slab, far tier, fluff); `FluidFieldGroundFog.{h,cpp}` (one solver, new settings) | `WindSolver.{h,cpp}`, `AddForcesCS`, `AdvectCS`, `DivergenceCS`, `JacobiCS`, `ProjectCS`, `AbsorbCS`, `WallsCS`, `FogCS`, `Levels.hlsli`, `FogPattern.hlsli` |

`ShallowWaterSolver.h` includes `WindSolver.h` only for `StepParams`, `NameFn` and `CompileFn`. Move those three into a
small `FluidField/SolverCommon.h` first, so Shallow Water builds unchanged; note that one-line touch in the commit.
Nothing else uses the wind solver; checked on 2026-11-11.

---

## Steps

Each step ends with a build, an in-game check by Victor, and its own commit. Measure GPU time standing and walking at the
test spot each time.

| Step                       | Contents                                                                                                                                                 | Accept when                                                                                                              |
| -------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| 1. Ground tiers            | `GroundTiles` (worker, budget, cache), resident texture, landscape fill, far height map, flags                                                           | Debug view correct at the Riverwood ledge spot; fog area complete instantly on arrival; main thread under 0.1 ms walking |
| 2. Fog solver              | `FogSolver` (three passes, one-way rocks, sources and evaporation, settled seeding, confinement); delete the wind levels; new `SolverTest` cases (below) | Tests pass on WARP; in game, pooling in hollows, 20 u flats, ledge fog spilling but never climbing, rocks clear          |
| 3. Draw                    | Column texture, slab march, far tier, max-mip bounds                                                                                                     | No arc, no fog hung across ledges, seamless sim-to-far; draw ≤ 0.65 ms                                                   |
| 4. Movers foundation       | Three-channel splat; contact map; bones at close range, capsules at mid                                                                                  | Walking parts the fog with leg-shaped holes up close and wakes further out; the ledge A/B test passes                    |
| 5. Fluff                   | 3D noise, profile, puff by depth, lift, wisps                                                                                                            | Victor judges real Z depth, not a sheet                                                                                  |
| 6. More movers             | Attacks, magic, projectiles, explosions, shouts, physics, in that order                                                                                  | Each one visible and within budget in a fight scene                                                                      |
| 7. Placement, timing, look | Fog-v2 sections 4 and 5: sources by water, swamp, forest and dungeon; weather and time; sun shadows, sky light, local lights                             | As fog-v2                                                                                                                |

**`SolverTest` cases (WARP, CPU reference):**

1. With no sources, evaporation or movers, total fog is unchanged after N steps.
2. A bowl fills level and stops at Pool Depth.
3. A ledge: fog on top spills over the brink; fog on the floor never rises onto the top.
4. A rock cell: fog beside it never enters; fog from a higher neighbour lands on it and drains off.
5. A push conserves fog: it piles in front and thins behind.

**Budget:** 1 ms GPU in total at Riverwood, standing and walking.

- Rough targets: solver ≤ 0.15 ms, splat and contact ≤ 0.1 ms, draw ≤ 0.65 ms, uploads near 0.
- Main thread ≤ 0.1 ms; the tile worker is separate.
- These are estimates. Report the measured numbers, and say plainly if a step goes over.

---

## Testing in game

- **Spots:** record a new save at the Riverwood ledge used on 2026-11-11, `FluidField_FogLedge`, in
  `C:\dev\cs-deploy\tools\test_spots.json`.
- **Standard conditions:** 12:30, clear weather (send `fw 81a` twice after a load), timescale 0.
- **Ledge A/B test** (as on 2026-11-11):
    1. Victor stands on the ledge, camera looking down at the floor below. Shot A.
    2. He walks along the edge, then takes shot B.
    3. Repeat with the debug view on and off.
    4. Do the same from the floor.
- **Fast travel test:** arrive at a foggy spot and check the fog is settled at once.
- **Weather test:** switch to a foggy weather and check the fog gathers in low spots first.
- **Build and launch:** `REPO=/c/dev/skyrim-community-shaders-fog bash build.sh`, and the same `REPO` for `launch.sh` and
  `fxc2.sh`'s `ROOT`. The staging folder is shared with the waterfall work: rebuild before testing.

---

## Working rules

- **Ask before restarting or closing Skyrim while Victor is in it or looking around.** Don't drive his camera or teleport
  him mid-look. Save his view before a restart and restore it after.
- When a script relaunches the game, make sure it's finished (fog on, time and weather set, camera restored) before
  handing over. Say when it's ready.
- Never leave a debug view on without saying so.
- Pixel-shader-only changes can be swapped into the running game. Compute-shader and DLL changes need a restart.
- Comments are the shortest clear message: why and constraints only, no history (memory `fluid-field-comment-style`).
- Commit locally per step and never push. Keep formatter churn in its own commit.
- Report honestly: measured versus estimated, seen in game versus only built.

---

## Open

- **Pipe model look:** if wakes look too smooth even with confinement, switch the solver to full shallow-water equations
  (same state plus advection).
- **Brink spill:** it stays. Revisit only if it's hard to make look good.
- **Overhangs:** two levels per column (D195), replacing one.
- **Interiors and dungeons:** no landscape or world height map. Ground from the mesh tiles alone (fog-v2 section 4 has the
  dungeon list).
- **Water:** still water counts as ground at its surface, with a strong source; fast water gets strong evaporation. That
  belongs to step 7.
- **Coexistence:** agreed change (Victor, 2026-11-12): coordinate with Exponential Height Fog (action plan items 18-23);
  back off only for GroundFog (Nexus 198571).

---

## Action plan 2026-11-12

**For:** the agent continuing Ground Fog on Windows, in worktree `C:\dev\skyrim-community-shaders-fog`, branch
`fluid-field/fog-v3` (last commit `034618321`).

**Source:** a review of that commit's code and the 2026-11-11/12 session, and Victor's selection and order of the items
on 2026-11-12. Nothing here is built.

**Plan record:** add this as a section of `docs/fluidfield/plans/fog-v3.md` ("Action plan 2026-11-12"), in its own docs
commit.

**Order:** Victor's (revised 2026-11-12): correctness first, then the optimisations (draw, sim, CPU), then the missing
features. The draw items (3–4) should bring the fog back under its 1 ms budget before the features add cost. Report the
measured cost after every item.

**Rules:**

- Follow `fog-v3.md`. Explain before changing anything; warn Victor before deviating, even when he asks for the deviation.
- Ask before restarting Skyrim while he's in it. Never leave a debug view on without saying so.
- Report measured versus estimated, and seen in game versus only built.
- Commit locally per item. Never push.
- Staging still has **Waterfalls and Shallow Water switched off** (crash isolation). Leave them off for fog work unless
  Victor says otherwise, and remind him before any waterfall session.

---

### 1. Ground fallback in the sim (review problem 1)

**Problem:** where a ground tile isn't resident, `FogBedCS.hlsl` falls back to the Terrain Shadows far height map
(`FarHeight`). That map is quantised in ~2,056-unit steps. The draw falls back to the loaded landscape (`LandMap`,
32 u), so after a load and at the tile window's edge the sim runs on wildly wrong ground until tiles land.

**Do:**

- In `FogBedCS`, fall back to `LandMap` (the same lookup as the draw's `LandGround`: height, and the flags for fog may
  form and water). Closed only where neither exists.
- Remove the far height map entirely: the `FarHeight` binding, `FarScale`/`FarOffset`/`FarZ`/`HasFar` in both shaders'
  constants, the unused `FarGround` in `GroundFogCS.hlsl`, and its loading in `GroundTiles` / `FluidFieldGroundFog`.
  Nothing else uses it. Check with a search before deleting.
- Bump the tile cache version only if the tile format changes; it shouldn't.

**Accept:**

- A new `SolverTest` case: with no tile resident, the bed equals the landscape map's height.
- In game, after a fresh load (`coc Riverwood`), fog lies on the right ground from the first second, with no sliding
  or reshaping as tiles arrive.

### 2. Ray bounds from a max mip (review problem 2; also draw optimisation 2)

**Problem:** `FogOverRay` clips the fog band from the ground under the camera and under the ray's end only. A rise
between them, higher than both, gets its fog cut out. That's the moving-arc failure from v2.

**Do (the plan's max-mip bounds; one implementation covers both review items):**

- **Build the mip:** after `FogDepthCS`, a pass builds a max mip chain of the fog's top, `bed + depth × FLUFF_REACH`,
  over both levels, per cell. Down to about 8×8 cells a texel. Past the sim grid, use the settled-fog top over the tiles
  and `LandMap`.
- **Use it in the march:** walk the ray on a coarse mip to find the intervals where it's below the fog's top, then march
  only those. A cheaper first version: about 8 probes along the ray on a coarse mip to set the band top, as v2's fix
  did, then the full interval walk.
- The steps go into the intervals actually inside the fog, so fewer are wasted.

**Accept:**

- At the Riverwood ledges and on a crest between the camera and distant fog: no arc, no clipped fog on rises, from low
  camera angles too (Victor's checks).
- Draw time measured before and after at `FluidField_FogBattle` (standing, same camera).

**Outcome (2026-11-12, agreed deviation):** the first version only. The probes keep the draw at its old cost (0.87–0.90
ms; the top map adds 0.066 ms). The full interval walk drew the same picture for 0.22 ms more (1.09–1.12 ms): the walk
itself costs about 0.12 ms, and the samples it skips, in clear air, were already cheap. Victor chose the probes.

### 3. Lighting per cell (draw optimisation 1)

**Problem:** every march sample reads Volumetric Shadows' shadow map and evaluates Skylighting's probes. That's tens of
millions of evaluations per frame, and the main reason the draw went from 0.62 to 1.03 ms.

**Do:**

- **A light pass:** after `FogDepthCS`, one evaluation per cell per level at the fog's mid-height (`bed + 0.5 × depth`):
  sun visibility (`VolumetricShadows::GetVSMShadow2D`) and sky visibility (Skylighting). Stored in a small texture per
  level (RG8 or RG16F).
- **The march** reads it bilinearly, level-matched like the column, instead of evaluating per sample.
- **Unchanged:** the self-shadow (per sample, cheap) and the local lights (once a ray).
- **Interiors:** sun off; sky from the cell's ambient (item 9).

**Accept:**

- Light shafts through trees still read at the 16-unit cell size (Victor's look at the battle save at dawn or dusk).
- Draw time measured before and after.

### 4. One draw texture per cell (draw optimisation 3)

**Problem:** per sample, `Ground` reads five 8-unit ground texels with slot checks (plus `LandMap` past the tiles), and
`FogColumn` reads twelve bed and column texels with level matching.

**Do:**

- Have the solver's output pass write, per cell and level, everything the march needs, already resolved: floor height,
  depth, velocity, churn, the fog-may-form and carries shares, and the ceiling under an overhang.
- The march reads that texture (bilinear, level-matched) and the noise, plus the 8-unit ground only where it keeps edges
  sharp: near the ray's end (the existing ground fix), and as one nearest-texel level check instead of five texels.

**Accept:**

- No visible stair-stepping or seams at ledges and overhang lips compared with before (side-by-side shots at the
  Riverwood ledges).
- Draw time measured. Target: the whole fog back under 1 ms GPU at `FluidField_FogBattle`.

### 4a. Banding along the wake's walls (added 2026-11-12)

**Problem:** running through the fog leaves a trench whose walls show streaky, layered bands (Victor's shots,
2026-11-12, at `FluidField_FogBattle`). The self-shadow change of 2026-11-11 (`min(depth, top)`, the sun slant floor) did
not cure it. Cause unknown. Candidates: too few samples across a steep, thin fog wall (the steps grow away from the ray's
ends); the self-shadow; steps in the sim's depth along the trench edge, a cell (16 u) each.

**Do (after items 3 and 4, on the draw they leave):**

- **Diagnose first:** one announced hot-swap at a time in the game: self-shadow off; then twice the steps; then a debug
  colour per sim cell. Explain the cause and the fix to Victor before changing anything.
- Then fix it at its cause.

**Accept:**

- Running through fog at the battle save: the wake's walls read smooth, near and far (Victor's check).
- Draw time measured before and after.

### 4b. Pixelated wisps, fog top and edges (added 2026-11-12)

**Problem:** the wisps and the fog's top layer look pixelated in game: speckled, dotted texels instead of a soft fade.
It shows most against things standing in the fog: round the moving player, and on vegetation and shrubs (Victor's shot,
2026-11-12, at `FluidField_FogBattle`). Likely causes, unconfirmed: the half-resolution march's per-pixel jitter not
resolved (the composite's 3 × 3 blur rejects texels across a depth edge, and thin geometry is mostly edges); the
nearest-of-four depth the march takes for each half-resolution pixel; and the wisps' hard noise threshold.

**Do (after 4a, on the same draw):**

- **Diagnose first,** as 4a: announced hot-swaps one at a time (no jitter; full-resolution march; softer wisp threshold)
  to find which part makes the dots. Explain before fixing.
- Then fix it at its cause: a smoother fade at the wisps' and top's edges, a jitter the frame's TAA resolves, and an
  edge-aware upsample that keeps fog soft round thin geometry without spilling over near surfaces.

**Accept:**

- The fog top and wisps fade softly, round the moving player and through grass and shrubs, near and far (Victor's
  check, moving and still).
- No fog spilling over near surfaces' edges (the reason for the nearest-of-four depth).
- Draw time measured before and after.

### 4c. Stirred fog keeps its surroundings' shading (added 2026-11-12)

**Problem:** fog lights up a little where an actor moves through it, even in shade (Victor's shot SS 2537, 16:30
clear, at `FluidField_FogBattle`). Not the sun: two of the draw's shading terms follow the stirred fog. Lifted fog and
wisps sit high in the layer, which is lit up to about 40% brighter than its bottom (`TOP_LIGHT` against `BOTTOM_LIGHT`);
and the self-shadow counts the fog over a point, which a wake thins and lifted wisps rise above.

**Do (Victor's choice of fix, option A):** the self-shadow and the bottom-to-top light follow the undisturbed fog round
the stir (its settled depth), not the stirred depth, so a wake doesn't glow. Explain the exact change before making it.

**Accept:**

- Running through fog in shade: no glow round the actor; the fog stays as dim as the fog round it (Victor's check).
- Sunlit fog and still fog look as before (side-by-side shots).
- Draw time measured.

### 4d. Actor shadows in the fog (added 2026-11-12)

**Problem:** since item 3, the sun's shadow in the fog is per 16-unit cell, so an actor's shadow (30–50 units wide)
shows at best as a blocky smudge. Unknown: whether Volumetric Shadows' shadow map holds actors at all (test swap,
2026-11-12: the fog coloured by the per-sample sun visibility).

**Do:**

- **If the shadow map holds actors:** within about the contact radius of the camera, the march's samples read the
  shadow map themselves again (as before item 3); past it, the per-cell light.
- **If it doesn't:** explain to Victor what it would take (likely our own shadow from the actors' capsules, which the
  contact map already has) and let him decide.

**Accept:**

- The player's and nearby actors' shadows read in sunlit fog, moving with them (Victor's check at 16:30 clear).
- Draw time measured before and after.

### 4e. Fog lit and coloured by Volumetric Lighting (added 2026-11-12)

**Problem:** with CS's Volumetric Lighting on, the air shows sun shafts and takes the sun's colour, but our fog keeps
its own, mostly sky-lit look: it doesn't scatter the sun the way the air round it does (Victor, 2026-11-12, 16:30 clear).

**Do:**

- **Research first:** what Volumetric Lighting computes and exposes (its in-scattered light, its density and colour,
  and where and when it's available to our draw). Explain to Victor how the fog would take light and colour from it, and
  the cost, before building anything.
- When it's on, the fog's sun light and colour follow it; when it's off, the fog keeps its own lighting. Little extra
  cost: read what Volumetric Lighting already computed rather than lighting the fog again.
- Fold in the sun's share in the fog (`SUN_SHARE`, `SKY_SATURATION`, `SUN_MULTIPLE`) only as far as needed to match.

**Accept:**

- With Volumetric Lighting on, at 16:30 clear and at dawn: sunlit fog scatters and takes the sun's colour like the air
  round it, shafts carry into it, and shade stays dim (Victor's check).
- With it off: the fog looks as before.
- Draw time measured with it on and off.

### 4f. Rain dents where the rain decals land (added 2026-11-12)

**Problem:** the sim's rain dents are random per 16-unit cell, so they don't match Wetness Effects' splashes and ripples on
the ground (Victor's shot SS 2548). Wetness Effects doesn't spawn decals: its shader places one possible drop per
4-unit grid square from a hash of the square and the time (`GetRainDrops`, `RaindropGridSize`, `RaindropChance`,
interval, lifetime), too fine for the sim's cells.

**Do:**

- In the draw, within about 15 m of the camera while it rains: each fog sample checks the same drop hash as the decals;
  over a live splash, a small hole in the fog that fades with the splash's own lifetime. The sim's per-cell rain
  thinning stays.
- Explain the exact change before making it.

**Accept:**

- In rain, fog dents appear at the decals' splash spots, at the same moments (Victor's check).
- Draw time measured in rain, before and after.

### 5. Ambient drift from a texture (sim optimisation 6)

**Problem:** `FogInputsCS`' `AmbientDrift` evaluates about 64 hash-noise lookups per cell per level per frame.

**Do:** compute the drift (curl of the two eddy scales) into a small texture (for example 128² over the grid) every few
frames, or derive it from the existing noise textures. Inputs reads it bilinearly.

**Accept:** the same drifting look (Victor's check), and the Inputs pass time measured before and after.

### 6. Skip lower levels where there are none (sim optimisation 7)

**Problem:** the lower level (D195) is dispatched over the whole grid, though most columns have no second level.

**Do:**

- **Quick part:** early-out at the very top of each pass for closed lower beds. `FogDepthCS` currently computes inflow
  before discarding.
- **Full part:** the bed pass marks 8×8 groups that contain any lower bed and builds an indirect dispatch list for
  level 1 in the Inputs, Flux and Depth passes.

**Accept:** `SolverTest` still passes (all 12 cases), and the per-pass times are measured before and after.

### 7. Cache bones per actor (CPU optimisation 8)

**Problem:** `AirSources` looks bones up by name (`GetObjectByName`) every frame for every nearby actor. The creature-leg
cache was removed over a dangling-pointer worry.

**Do:**

- Per actor, keep the 3D root and the found bones as `NiPointer`s. That keeps them alive, so nothing dangles.
- Invalidate when `Get3D()` returns a different root, or the actor unloads.
- Look up by name only on a miss.

**Accept:** movers unchanged in game, and `AirSources::Update` time measured in the battle scene before and after.

### 8. Measure and trim the landscape depth pass (CPU optimisation 9)

The second landscape draw is ours alone: `LandscapeDepth`, used only by the distant fog, and only while the fog is on.

**Do:**

1. **Measure:** GPU timestamps around our extra draws, and CPU time in the hook, at the battle save and in an open
   valley view.
2. **Trim:**
    - draw only distant LOD landscape passes (the LOD-landscape technique), since near landscape is covered by the tiles
      and `LandMap`;
    - skip the pass entirely when Distant Fog Reach is 0, or in interiors.
3. **Check** with Terrain Blending on and off, since our hook chains after its.

**Accept:** the distant fog unchanged (far shore lit, no fog on water, trees or cliffs), and the measured cost before and
after reported.

### 9. Interiors and dungeons (review missing 3)

**Goal:** fog in the dungeon kinds `fog-v2.md` section 4 lists: caves, Nordic ruins and barrows, Dwemer ruins, ice caves
and mines.

**Do:**

- **Ground:**
    - interiors have no landscape, so mesh tiles alone, read from the interior cell's statics with the same
      classification;
    - key the tile cache by the interior cell's form ID instead of the worldspace;
    - no `LandMap` and no distant fog in interiors.
- **Where it forms:**
    - only in dungeons, decided by the location's keywords plus a curated list (verify the keywords in game, for example
      `LocTypeDraugrCrypt`, `LocTypeDwarvenAutomatons`, `LocTypeMine`, `LocTypeAnimalDen`, `LocTypeDungeon`);
    - per-kind amounts (thicker in barrows and caves, thin in Dwemer ruins) as data, not code.
- **Lighting:** no sun or sky in interiors. Local lights carry it (torches, braziers), plus the cell's ambient light.
- **Look:** a per-kind tint and density, with the dungeon list in JSON next to the weather rules from item 11.

**Accept:**

- Fog in one barrow (Bleak Falls Barrow), one cave and one mine, lying on floors and pooling in low rooms.
- None in houses or shops.
- Tiles cached and instant on re-entry.
- Cost measured in a lit barrow corridor.

### 10. Physics clutter as movers (review missing 4)

**Goal:** objects knocked or thrown (clutter, baskets, bodies in ragdoll already done) push and cut the fog.

**Do:**

- Every few frames, collect references with dynamic Havok bodies inside the sim grid, moving faster than a threshold,
  capped by distance × speed in the existing ranked mover list.
- Each becomes a pusher, and a contact capsule within Contact Radius, from its bounds and linear velocity.
- Prefer events where possible (hits, grab and release) to start tracking, rather than a full reference scan every
  frame.

**Accept:**

- Throwing or knocking clutter through the fog parts it.
- At rest it doesn't cut (like the stuck-arrow rule).
- The CPU cost of the scan is measured in the battle scene.

### 11. Placement and timing (review missing 1; fog-v2 section 4, carried into fog-v3)

**Goal:** fog that belongs to the place, the weather and the hour, not one amount everywhere.

**Do:**

- **Fogginess (0–1) per frame, scaling Ground Thickness and Pool Depth:**
    - weather: its own fog distances first (works with any weather mod), then rules by weather name, then per-weather
      overrides in JSON;
    - time of day: thickest just after dawn (about 06:30), present at night. Away from foggy weather, it's mostly gone
      from dry ground by midday. **In a foggy weather the time of day only thins it a little and never clears it** (no
      morning burn-off; Victor, 2026-11-12). The weather sets the floor; the hour only trims above it;
    - wind and season thin it;
    - **still water at dawn:** a thinner layer over still water (about half Ground Thickness), peaking at dawn and fading
      through the morning, on top of whatever the weather gives.
- **Rain:** keep today's rain dents as they are. No further rain work (Victor, 2026-11-12).
- **Sources, combined by max (never summed), on the bed's fog-may-form share:**
    - **still and slow water:** let fog form and be drawn over lakes and ponds, using `WaterSurfaces`' flow scale (flowmap
      types are rivers). Fast rivers keep evaporating, so rapids stay clear;
    - **cold hollows:** the existing hollowness term, weighted by night and calm;
    - **dense forest:** tree references per tile, counted where `GroundMeshes` already classifies trees;
    - **swamps:** a curated region and worldspace list in JSON (Hjaalmarch, Solstheim's marshes), plus auto-detection
      (shallow water mixed with marsh and mud land textures);
    - **dungeons:** from item 9.
- **Never abrupt:**
    - targets ease over Ease In;
    - a weather change blends over its transition, with fog forming on slopes and draining so low spots fill first;
    - **valleys first, made explicit:** while fog is arriving (a weather change into fog, or dusk), weight the sources by
      hollowness, so valley floors and hollows visibly fill before slopes and open flats catch up;
    - fast travel, loading and teleports seed the settled field, as now.

**Accept:**

- Over one in-game day at Riverwood in clear weather (timescale raised for the test): dawn fog in hollows and a thin
  layer over the river's still stretches, gone from the flats by noon, back at dusk.
- The same day in a foggy weather: only slightly thinner at noon, never cleared.
- A foggy weather fills low spots first.
- Lake Ilinalta gets fog over the water, the river rapids don't.

---

## Improvements (after items 1–11)

Victor's picks from the review's "ways to make it more interesting", 2026-11-12. Same rules as above: explain before
changing, warn before deviating, measure every item, commit each on its own. Items 16 and 17 need fog over still water,
which item 11 brings, so they come last.

### 12. Landing from a jump stamps a ring

**Goal:** an actor landing in the fog throws it outward in a ring, which then flows back. Victor: "THANK YOU, that has
been driving me crazy".

**Do:**

- **Detect landings** for the player and NPCs in the grid: falling to grounded, from the character controller's airborne
  state (verify the CommonLib call), with vertical speed going from falling to about zero as the fallback. Record impact
  speed and position.
- **The stamp:** a short radial push outward (a few frames), scaled by impact speed and actor scale. Fog piles in a ring
  and the centre clears for a moment, then the flow fills it back in. A churn kick at the ring lifts it into wisps.
- **Close range:** the landing feet also cut through the contact map, as they do now.

**Accept:**

- Jumping into fog leaves a visible ring that refills over a second or two.
- A taller fall gives a bigger ring.
- NPCs landing (jumping down ledges) do it too.

### 13. Running kicks wisps up sideways and behind only

**Goal:** a running actor kicks the fog up beside and behind them, never ahead.

**Do:**

- **Today:** churn (which drives the draw's lift and wisps) is just the flow's speed.
- **Add a kick:** a decaying per-cell kick, splatted for actors above a running speed only where
  `dot(outward, heading) < 0.3`, so beside and behind them. It fades over about 1 s.
- **Storage:** a fourth Force channel, or a small per-level kick texture. The column's churn becomes
  `max(flow speed, kick)`.
- **The bow wave ahead** stays a push only, not a lift.

**Accept:** running through fog leaves lifted wisps trailing behind and to the sides, nothing kicked up ahead, and
walking doesn't kick.

### 14. Frost spells only push

**Today:** fire, frost and shock projectiles thin the fog a little under their path ("heat").
**Change:** frost only pushes the fog out of the way, with no thinning. Fire and shock stay as they are.
**Accept:** an Ice Spike or Frostbite through fog parts it but leaves its amount unchanged once it flows back.

### 15. Dragon wings on take-off and landing

**Goal:** a dragon taking off or landing blasts the fog outward with its downdraft.

**Do:**

- **Detect:** dragons by race or keyword (`ActorTypeDragon`), and their take-off and landing from their flight state
  (verify the CommonLib accessor), or from vertical speed near the ground.
- **The downdraft:**
    - a wide radial push centred under the body (several hundred units), lasting about 1–2 s;
    - a cut under the body;
    - a churn kick at the rim, so the fog billows up there.
    - Optionally, weaker pulses while hovering low.
- Goes through the ranked mover list like every other mover.

**Accept:** at a dragon fight (a dragon spawned near Whiterun or Riverwood, with `tgm`), take-off and landing visibly
blow a clearing that refills. Cost measured during the fight.

### 16. Swimming churns both the fog and the water

**Needs:** fog over still water (item 11), and **Shallow Water switched back on** in staging for this test (Victor's OK
first, since it's off for crash isolation).

**Do:**

- **Swimmers and waders** push and churn the fog over the water they're in. The movers already exist; make sure they
  act on water cells' fog.
- **Water drives the fog:**
    - when Shallow Water is active, the fog's Inputs pass reads its flow texture, sampled by world position, since its grid
      differs from the fog's;
    - fog over water is dragged by the water's velocity (a share of it), and churned where the water is turbulent (shear),
      so a swimmer's wake stirs both.
- **Optional:** without Shallow Water, the fog works as before, so the binding is optional.

**Accept:**

- Swimming across a still pool at dawn churns the mist above and leaves a wake in both.
- With Shallow Water off, nothing breaks.

### 17. Fire on water makes steam

**Goal:** fire meeting water throws up rising steam.

**Do:**

- **Detect:**
    - fire projectiles, explosions and beams (fire damage, for example by `MagicDamageFire` or the effect's resist value)
      hitting or passing low over water, checked against `WaterSurfaces`' height at the point;
    - burning objects standing in water.
- **The steam:**
    - a short-lived fog source on those water cells (a new add-fog splat channel);
    - strong churn and lift, so it rises in wisps;
    - a slightly brighter, whiter look;
    - it evaporates quickly unless the weather is foggy.
- Flames held over water make a continuous plume.

**Accept:** a Fireball into a lake makes a burst of steam that rises and fades, and Flames over water steam
continuously. Cost measured.

### Decided against, or later

- **Morning burn-off:** no. In foggy weather the hour only thins it a little; see item 11.
- **Rain:** today's dents are enough. No further work.
- **Lightning flashes lighting the fog:** not needed. Mods already add particle lights to spells, and the fog's local
  lights pick them up through Light Limit Fix.
- **Waterfalls feeding mist into pools:** later, once waterfalls are back in scope.

---

## EHF integration (after items 1–17)

From a review of Exponential Height Fog (EHF) on 2026-11-12, approved by Victor the same day: **coexist and coordinate,
don't merge.**

**Why:**

- EHF is the atmosphere: two exponential layers at **fixed world heights**, applied inside every material shader as each
  surface draws, plus an optional volumetric froxel fog with near and far grids.
- Ours is a fog layer measured **above the terrain**, composited once after the world.
- They're complementary. "Seamless" means agreeing on draw order, light, the distant hand-over and weather.

**Plan change:** fog-v2 section 5's "back off when Exponential Height Fog's volumetric fog or GroundFog is on" is replaced:
**coordinate with EHF (items 18–23); back off only for GroundFog (Nexus 198571), which stacks the same kind of fog.**
Record this in `fog-v3.md` as an agreed change (Victor, 2026-11-12).

**Ruled out for now:** putting our fog into EHF's froxel volume. Its 16-pixel near tiles and 64-pixel far tiles can't
hold a 20–40-unit layer: the top, the wisps and the contact cuts would be lost, and its frame-to-frame reuse would smear
wakes. Putting only the distant layer in is a possible later experiment, not part of this plan.

**Every item works with EHF off:** our fog behaves exactly as now. Test each item with EHF off, with EHF on, and with EHF's
volumetric fog on.

### 18. Respect EHF's fog in front of ours

**Problem:** EHF's fog is already baked into each surface when our composite runs, so ours is treated as nearer than all
of EHF's fog. Distant ground fog pops out crisp and bright against hazy mountains instead of sitting inside the haze.

**Do:**

- `GroundFogCS.hlsl` includes `ExponentialHeightFog/ExponentialHeightFog.hlsli`.
- **Once per ray,** at the fog's representative distance `d`, evaluate EHF there: its analytic fog plus its volumetric
  sample (`GetExponentialHeightFog` / `SampleVolumetricFog`, or their internals), giving its transmittance `T_ehf(d)` and
  the light it scatters in front of `d`, `L_ehf(0..d)`. The representative distance is the scatter-weighted point
  already computed for local lights.
- **Compose physically**, so EHF's haze in front isn't dimmed by our fog and ours is dimmed by it:
  `result = scene · T_ours + T_ehf(d) · L_ours + L_ehf(0..d) · (1 − T_ours)`.
- Same for the distant layer (`DistantFog`), at its surface distance.
- Bind EHF's integrated scattering textures (t19 and t22 in materials) to our draw at free slots, or reach the volume
  through EHF's feature, without changing EHF.

**Accept:**

- With EHF on, distant ground fog reads as inside the haze: dimmer and softer with distance, no bright band against hazy
  hills.
- Near fog is unchanged.
- EHF off: identical to before.
- Cost measured, about 0.02 ms expected.

### 19. Shared light with EHF

**Problem:** where ours fades into EHF the two are lit differently, so the hand-over shows as a shift in colour and
brightness:

- EHF uses sky IBL × tint, plus the vanilla fog colour's share, plus its own sun lobe;
- ours uses its own albedo, forward and backward sun lobes, and desaturated Skylighting colour.

**Do:**

- **With EHF on,** our sky term is built from EHF's inscattering inputs: sky IBL, or Dynamic Cubemaps in interiors; the
  inscattering tint; the vanilla fog colour amount; and its directional multiplier and anisotropy as the sun lobe's base.
- **Kept on top near the player:** our own detail (sun shadows, sky visibility, self-shadow, local lights).
- **With distance,** our colour blends fully onto EHF's own fog colour, so far ground fog is the same medium as the haze.

**Accept:**

- No colour or brightness step where our fog meets EHF, at noon, at dusk and at night.
- Victor's judgement on the near look, which should still read as our fog's lighting.

### 20. Hand the distant layer over by EHF's opacity

**Problem:** our distant layer fades out at a fixed reach (Distant Fog Reach, 90%), whatever EHF does at that distance.

**Do:**

- With EHF on, the distant layer fades out as EHF's opacity at that distance rises (from item 18's evaluation). It vanishes
  exactly where the haze hides it.
- **Remove the Distant Fog Reach slider** (Victor, 2026-11-12): its setting, its UI and i18n strings, and its constant.
  The reach is no longer a user choice:
    - **EHF on:** the fade follows EHF's opacity alone. The only hard limit is the landscape actually drawn
      (`LandscapeDepth::GetReach()`).
    - **EHF off:** a fixed fade over the last part of the landscape's drawn reach (today's 90% default, built in).

**Accept:**

- In a wide valley view (the battle save across the lake) with EHF on, there's no visible end to the ground fog.
- With EHF off, the fade looks as it does today at 90%.
- The slider is gone from the menu, and old saved settings load without errors.

### 21. Composite before effects

**Problem:** our composite runs after `Main_RenderWorld` returns. As far as the hook order shows, that's after particles
and effects (fire, spell particles), which write no depth, so our fog is laid over flames as if they stood at the opaque
surface behind them. EHF fogs each particle at its own depth.

**Do:**

1. **Confirm** with one RenderDoc capture where effects draw relative to our composite.
2. **If they come before it:** find a hook point after water but before the alpha effects pass, and composite there. Fire
   and spell particles then draw over our fog, and EHF keeps fogging them per pixel. Fog over water (item 11) still works,
   because water is already drawn.
3. **If no clean hook exists:** keep today's point and record why.

**Accept:**

- A campfire and a Flames spell in fog show bright flames over the fog, not fogged flames.
- Fog over still water is still visible.
- No change elsewhere.

### 22. One weather signal for both

**Problem:** item 11's fogginess and EHF react to weather separately, so in a transition one can thicken while the other
lags.

**Do:**

- Register Ground Fog's per-weather amount (and, if useful, Ground Thickness and Pool Depth scale) as CS weather variables,
  the way EHF's settings are (`RegisterWeatherVariables` / the weather variable registry; verify the API in this tree).
- Both features then follow the same weather blend, and the CS editor tunes them side by side.
- Item 11's "the weather's own fog distances first" becomes the default for that variable. Name rules and JSON overrides
  sit on top as item 11 describes.

**Accept:**

- A weather change into fog thickens both together, and back out thins both together.
- The amount is editable per weather in the CS editor.

### 23. Coexistence rules and guidance

**Do:**

- **No back-off for EHF.** Keep a back-off (fade to nothing) only when the GroundFog mod is detected (its DLL loaded).
- **A short guidance note** in Ground Fog's settings and in the docs: use EHF as the atmosphere (its layers well above the
  ground, no low ground layer) with Ground Fog for the ground.
- **A UI hint,** text only and never changing EHF's settings, when EHF's second layer looks like a ground layer (a high
  density with a steep falloff close to the terrain's height round the player).

**Accept:**

- With GroundFog installed, ours steps aside.
- With EHF set up as a ground layer, the hint shows.
- EHF's settings are never touched by Ground Fog.
