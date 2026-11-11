# Ground fog v2: levels, movers, placement, look (D193)

Goal: ground fog that every moving thing pushes and cuts (actors, blades, spells, projectiles, explosions, shouts, physics,
swimming), lying where it belongs (water, hollows, forests, swamps, some dungeons), draining down forested slopes, and lit
by Community Shaders' sun shadows, sky light and local lights. Budget: 1 ms per frame in total. Replaces the scope of
`fog-slice.md` (D21); its density layer, refill and ray march carry over. Waterfalls are left out for now.

Compared with GroundFog (Nexus 198571, 0.1.0) on 2026-11-10: same core (a 2D fluid fog layer, raymarched); it leads on
walls, valley pooling, weather/time rules and sim reach; we can lead on movers, the water coupling and lighting.

## 1. Sim levels

Three nested grids centred on the player, the same size each (cost per level fixed):

| Level | Grid | Cell | Reach | Movers in it                                 | Updates                                       |
| ----- | ---- | ---- | ----- | -------------------------------------------- | --------------------------------------------- |
| L0    | 128² | 12 u | ±11 m | skeletons, weapons, projectiles              | every frame, only while something moves in it |
| L1    | 128² | 32 u | ±29 m | a capsule per actor; projectiles, explosions | every frame                                   |
| L2    | 128² | 96 u | ±88 m | a blob per actor; explosions                 | every 2nd-4th frame                           |

- ~49k cells in all against today's 65k (one 256² grid at 16 u), for three times the reach.
- Each step a finer level takes its edges from the coarser one and writes back into it where they overlap, so a wake
  leaving L0 carries on in L1 at lower detail (no pop, no per-actor trails to keep).
- L0 idles when nothing moves within it: its last state settles into L1.
- Coarser levels need fewer pressure iterations.
- Settings: Fine Radius (L0's reach: changes its cell size, not its cost), Sim Levels (1-3), Far Update Rate.

## 2. Beyond the sim, walls and drainage

- **Ground**: one ground layer: the highest surface under each cell, landscape or a mesh on it (loaded cells near,
  landscape LOD far). A mesh on the landscape replaces the landscape's height there, never adds a second layer.
- **Far layer** (past L2): no simulation; the resting fog shaped by the ground: more in hollows (lower than the ground
  around), thinner on steep slopes, more on the lee of ridges (slope against the weather's wind). Drifts with the wind.
- **Walls**: steep ground blocks air **climbing** it (as the shallow water solver's obstacles); going down is free.
  Sheer rock faces stay walls from below.
- **Drainage**: dense fog gets a downhill push on L0 and L1 (cold air sinks), strongest in forest and swamp sources and
  at night or in calm weather: on a forested hillside it creeps down between the trees and pools at the foot (the
  'creepy forest' look) instead of being stopped by the slope.
- **Blend**: each level fades into the next over the last ~15% of its reach (density and wind).

Done 2026-11-11 (c18d91dfd): the ground rays take placed meshes (roads, dirt cliffs, floors) as ground and skip rocks,
logs, trunks, plants and clutter, recording their tops as walls (fine and medium levels); ground faces up; under an
overhang the floor is a second level; the draw does not ramp fog across steps and fades it on steep faces.

Next for overhangs: a fog layer of its own on the lower level, and fog flowing down from the upper level to the lower one
at the brink (the user's request, 2026-11-11).

## 3. Movers

Main thread, under a per-frame budget:

- One list per frame ranked by distance × speed, with a cap per kind. Movers down the list coarsen before they drop
  (skeleton → capsule → blob → none). Far movers refresh every few frames.
- Projectiles and explosions from hooks or events, not a scan of every reference.
- Physics bodies only over a speed threshold, inside L1, capped.

GPU: every mover goes up as a compact capsule or line; one pass per level draws them into a **force** texture and a
**carve** texture (fog cleared outright: a blade's swept path, a blast). The cost goes with area, not mover count; the
64-emitter cap goes.

In order:

1. Skeletons (feet, legs, hips; whole body for creatures), within L0 only. Exists (AirSources), moved onto the new pass.
2. Weapons, every actor's in L0 under the budget: the blade from its bone and bounds, its sweep since last frame a line that pushes along its motion and
   carves the fog (a slash cuts it).
3. Projectiles and spells (arrows too): small moving pushers; a burst where they hit.
4. Explosions: a radial burst and carve by their radius.
5. Shouts: a cone push.
6. Water: where the shallow water moves faster than the air it drags it; where it is turbulent (swimming, splashing) it
   stirs the air and lifts a little mist. One read of the water's velocity in the force pass; its grid aligned to (or
   resampled onto) L0/L1.
7. Physics bodies (ragdolls, knocked clutter).
8. Large creatures (dragon wings, horses).

## 4. Where and when

A fog source map: per cell, sources combined by **max** (never summed, so marsh texture, water and forest in one cell do
not stack), each weighted by weather, time of day and season:

- Still and slow water: lakes, ponds, slow pools (the water maps; the water's own speed thins it, so fast rivers and
  rapids keep mostly clear: their moving water disperses it).
- Cold hollows (the far layer's hollow term).
- Ground meshes (the one ground layer: fog lies on rocks and floors, not under them).
- Dense forest (tree references per cell in loaded cells).
- Swamps: a curated list of regions and worldspaces (Hjaalmarch, Solstheim's marshes; JSON) plus auto-detection
  (shallow water mixed with marsh and mud land textures), so modded swamps work too.
- Dungeons: caves, Nordic ruins and barrows, Dwemer ruins, ice caves and mines (location keywords; interiors have no
  landscape, ground from depth).

Weights: the weather's own fog distances first (any weather mod), then rules by weather name, then per-weather
overrides (JSON). Thickest just after dawn, mostly gone from dry ground by midday, a thin layer left over water; wind and
season thin it.

Never abrupt: every target amount eases in over Fog Ease In seconds; newly loaded ground and new sources fade in through
the refill; weather changes blend over their transition.

## 5. Look

- Sun shadows from the shadow map in the march (shafts through trees).
- Sky light from Skylighting's probes.
- Torches and local lights glowing in it (Community Shaders' light lists), with quality tiers.
- Linear Lighting matched.
- Settings after Exponential Height Fog: Density, Thickness, Albedo, Forward Scattering, Sun, Sky Light, Local Lights,
  Max Distance, Detail Noise, Ease In, a push strength per mover kind.
- Drawn at half resolution, upscaled.
- Backs off when Exponential Height Fog's volumetric fog or GroundFog is on (no stacked fog).

## Order and budget

1. Foundation: the three levels, idle L0, the force and carve textures.
2. Movers (section 3, in its order).
3. Walls, drainage and the far layer.
4. Placement and timing.
5. Look.

Each step is measured in a heavy scene (a fight at Whiterun's gate, a dragon) against its share: ~0.3 ms sim and movers,
~0.6 ms drawing, a little for the source maps.

## Open

- The landscape LOD source for the far layer. (Checked 2026-11-10: the fog's ground map is 512 × 16 u, ±58 m, landscape
  only, from LandscapeCache's loaded cells at 128 u spacing; loaded cells reach ±~146 m, so L2 is covered. Meshes on the
  ground are new work: the one ground layer.)
- (Checked: the shallow water's near grid is 1024 texels over 8818 u, ±63 m, and its far grid 8× that; both cover L0-L2,
  with different cells and scroll, so the force pass reads the water's velocity bilinearly in world space.)
- Each dungeon kind's look (survey in game).

Test scene for the movers: Riverwood (NPCs, the river for the swim coupling; the river itself should stay mostly clear).
