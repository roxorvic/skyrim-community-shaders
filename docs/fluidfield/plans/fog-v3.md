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
- **Overhang undersides:** dropped. Take the top surface, since there's one level per column.

**Building:**

- On a **worker thread** under a time budget, nearest first and biased toward the direction of travel.
- The main thread only copies the statics' triangles (as `GroundMeshes` does now) and later uploads finished tiles.
- **Disk cache:** per worldspace, keyed by the load order (plugin list hash) and the tile's coordinates. Written once, when
  a tile is first built, never during normal play.

**Resident texture:**

- The tiles round the player, mapped into one toroidal texture (for example 1,024² at 8 u = ±4,096 u). Scrolling copies
  tiles; nothing is rasterised.
- Where a tile isn't ready, it's filled from the landscape (tier 2) at once, and replaced when the tile arrives.

**Far ground:**

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
- **Overhangs:** one level per column, as accepted. Revisit only if Victor finds a spot where it matters.
- **Interiors and dungeons:** no landscape or world height map. Ground from the mesh tiles alone (fog-v2 section 4 has the
  dungeon list).
- **Water:** still water counts as ground at its surface, with a strong source; fast water gets strong evaporation. That
  belongs to step 7.
- **Coexistence:** back off when GroundFog (Nexus 198571) or EHF's volumetric fog is on (fog-v2 section 5).
