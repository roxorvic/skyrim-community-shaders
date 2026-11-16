# Shallow Water on PBR Water: our sim drives his water

Branch `fluid-field/water-pbr`. Brief: `D:\SHALLOW_WATER_BASE_PBR_WATER.md`. Design agreed with Victor 2026-11-13;
replaces the earlier "river surface terms" plan, whose rounds are summarised under History.

## Goal

Inside the area our shallow-water sim covers, Davo's tessellated PBR water takes its shape and its current from our
sim. His waves and ripples stay on top as small detail, carried and scaled by the sim. Outside that area (the sea,
distant water, LOD, interiors) his code runs exactly as before. Lakes may get the sim later; discussed separately.

## Rules

- **Every change to his code is blended by our coverage** (0 outside the sim, on dry cells and at the area's edge),
  so water outside the sim is untouched.
- **The sim's data goes in as it is.** No effects stacked on top to hide its errors; what looks wrong is fixed in the
  sim and measured with the probe (Shallow Water `DebugProbe`). Effects are added back only where the sim truly cannot
  make something.
- **Never into an obstacle:** water may meet, climb or run over a rock, never slope down into it or show through it.
  Dry cells beside water carry the water's level; near walls the water dips at most ~5 cm below vanilla's level.

## His assumptions vs a shallow-water sim, and the change for each (inside our area)

| Area               | His code assumes                                                                                          | Change                                                                                                                                                                              |
| ------------------ | --------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Height             | Waves and ripples around a flat plane per draw                                                            | The surface sits at our sim's true height above the draw's own plane (`RippleDisplacement` backed by our surface, no sideways push); his waves on top                               |
| Depth              | LAND terrain only (`ctx.depth`, `elevation`): caps ripples, shoals and damps the FFT, lifts the waterline | Our sim's depth (terrain + bed rays + mesh cuts), exported by `WaterOutputCS`                                                                                                       |
| Current            | Flowmap (`FlowVelocity`), opposite sign to ours (open, see below)                                         | Our sim's velocity into `ctx.flow`, so his FFT is carried by the real flow                                                                                                          |
| Small waves        | FFT by fetch and wind, damped on rivers by the flowmap                                                    | Same FFT, carried by our flow and scaled by the sim's state (speed, depth, roughness): calm pools stay calm, rapids lively                                                          |
| Tessellation       | Screen-size density, x2 in his ripple grid and shore field                                                | Refine by our slope and curvature (rocks, fronts, drops), capped by his max factor; measured against the even density, the cheaper at a similar look wins. The forced-on hack goes. |
| Lighting           | Ripple slope added in full; vanilla detail normal scaled by wind and calm                                 | Our slope enters as the ripple slope (full strength); `ApplyWaterNormal` leaves the PBR path                                                                                        |
| Ripples            | His 512² ripple sim with actor and physics-object sources                                                 | Off in our area now (Victor 2026-11-13); his physics-object sources and silt ported into our sim later                                                                              |
| Displacement bound | `DisplacementLimit` and frustum margin sized for waves                                                    | Include our sim's height range                                                                                                                                                      |
| Motion vectors     | Previous copy of his ripples (t113, `Ripple2`)                                                            | A previous copy of our surface and grid placement; last                                                                                                                             |

**Removed from our side:** the local-average height, upwelling, standing waves, flow relief, the climb and sheet caps,
the forced tessellation, `FluidHeight` as a separate add-on. `WaterOutputCS` keeps data preparation only: true height,
level carried into dry cells, the near-wall floor, coverage, slopes, depth, flow, foam.

**Not wanted:** standing-wave trains on rivers (Victor: not needed). Hydraulic jumps and pile-ups come from the solver.

## Steps

1.  **Current direction (read-only first).** The same Unified Water flowmap is decoded `-(rg*2-1)` by PBR Water
    (`FlowVelocity`, after CS's raindrop ripples "180° correction") and `+(rg*2-1)` by our sim (`LoadRiverFlow`).
    Settle which is the water's travel direction from vanilla's shader, the engine and our test history; check in game
    with a direction arrow next to a falls if still unclear.
    **Measured 2026-11-13 (probe, PBR 2):** at all ~320 vanilla water steps in the sim's area the sim's water runs
    downhill 1.7-5 m either side, at ~3 m/s, whether Current Pull is 0, 0.25 or 2. So the flowmap barely steers our
    sim; gravity down vanilla's stepped planes does, and the sign question is moot inside our area once step 3 feeds
    our velocity to his waves. His `-` decode still matters outside our area: settle it with a direction arrow later.
    The same finding is step 5's main problem: steps drive the water at ~3 m/s, faster than the river's current.
2.  **Ripple slot and output cleanup:** back his ripple functions with our sim, his ripples off in our area, our depth
    exported, the extras removed. Check at the PBR cameras with the probe and the height views.
3.  **Current and small waves:** `ctx.flow` from our sim; FFT scaled by the sim's state.
4.  **Tessellation:** slope-driven refinement, measured against even density.
5.  **The sim's own problems,** measured with the probe: water up to 6 m/s in stepped stretches, the falls' jet, energy
    handed back as height behind rocks, ±2 m outliers.
6.  **Motion vectors.**
7.  Later: physics-object sources and silt in our sim; our foam into his foam renderer; CPU height for buoyancy and the
    camera; lakes.

## Progress (2026-11-13, late; built, not yet run in game: the GPU limit resets on reboot)

- **Step 2 done:** `WaterOutputCS` is data preparation only (true height above vanilla, level carried into dry cells
  under the rock's top, 5 cm floor near walls, full slope, coverage, foam, depth in flow.w). His ripples give way in
  the sim's area and the sim's surface is added in `DisplaceSurface`; `ShadeSurface` takes the sim's slope in place of
  his ripple slope; `ApplyWaterNormal` is off under PBR. Blended at his three call sites, not inside his ripple
  functions: his callers scale ripples by river damping and his terrain depth fade, which must not touch the sim's
  surface. `FluidField::AreaWeight` (the sim's share, faded only at the area's edge, not at rocks) does the blend.
  Removed: FluidHeight add-on, local mean, upwelling, standing waves, relief, climb and sheet caps, tess hack, debug
  view "River Surface". Debug views now: 15 height layers, 16 sim height (white above 1 m), 17 sim depth.
- **Step 3 done (code):** `GetWaveFlowDamping` gives his waves the sim's velocity and damps them by its speed (full
  at River Current Speed) in the area; `BuildWaveContext` takes the sim's depth there (0 over dry rock tops).
- **Step 4 done (code):** `EdgeTessFactor` refines the sim's area like his ripple grid, up to 4x more where its slope
  reaches 0.2; Shallow Water setting "Adaptive Tessellation" (on) turns that off for the cost comparison. C++ tessellates
  draws whose bounds reach the sim's area (`FluidFieldShallowWater::Covers`).
- **Step 6 done (code):** `previousSurface` copy each output write, last frame's placement in `PrevGridMin`, bound at
  t70/t71 for VS/DS; the stencil pass's previous position uses it.
- His files test `FLUIDFIELD_WATERSURFACE_HLSLI` (our header's guard), so his compute shaders compile whatever the
  global defines. Checked: 339 water variants with Fluid Field, 170 without, all his compute shaders both ways.
- **Risk to check first in game:** the sim's height is against its own vanilla level per cell, added to each draw's
  plane. Where two vanilla planes overlap at a step, a draw whose plane differs from the sim's is offset by the
  difference. Fix if seen: store the sim's vanilla level at full precision and measure against the draw's plane.
- **First in-game checks:** the PBR 1-4 shots (normal, 15, 16, 17), the probe at PBR 2 and 3, Adaptive Tessellation on
  vs off (GPU timers), river steps for doubled surfaces, motion smear while turning.

## For Victor to review (decided while he was away, 2026-11-13/14)

- **Gravity on the deviation from vanilla's level (option A).** Between two cells that both have vanilla water,
  `WaterVelocityCS` accelerates by the water's height above vanilla's level, not its absolute height. Why: the sim is
  pinned to vanilla's flat planes (LevelRelax), which step down along a river, so it pumped water down every step at
  ~3 m/s (probe: same speed with the flowmap current off); water then piled 0.5-1 m against rocks. Now the river's
  current comes from the flowmap; waves, wakes, pile-ups, climbing and draining at banks and rocks keep true gravity.
  Cost: water no longer speeds up by itself at a drop; rapids get their speed only from the flowmap current.
  Alternatives not taken: (B) weaker pinning so the sim conserves water (levels drift, more work); (C) Manning bed
  friction (barely slows deep pools; worth adding later for shallow rapids).
- **Correction to the above:** the probe after option A showed the same ~3 m/s, so vanilla's steps were not the
  driver. The sim's velocity decay relaxes toward the flowmap's current even with Current Pull 0, so the earlier
  "pull 0" test never removed it. With gravity at the steps gone, the flow still runs downhill at 320 of 324 steps:
  **our flowmap decode (+) is right; PBR Water's `-(rg*2-1)` runs upstream** (tell Davo). Option A stays (sound, and
  no longer pumps), and the real fix for the speed is next:
- **Current scaled by depth:** the flowmap's current is flat 2D, so a 2 m pool ran as fast as a shallow rapid. It is
  now scaled by 0.5 m over vanilla's depth (0.25x to 1.5x): the same water through every cross-section.
- **Films on rock tops:** water on a bed above vanilla's drains fast unless it runs faster than ~0.85 m/s (a sheet);
  films under ~4 cm are not shown as a surface (`SHEET_MIN_DEPTH`). Removed the jagged walls at the rocks.

## Root causes found 2026-11-15 (falls area, view 15/16 + probe)

The probe said the sim stands only ~5 cm above vanilla near rocks, yet view 16 showed 0.5-1 m over rocks, banks and at
the falls. So the domes, plateaus and the falls tent were not the sim: they were what the output pass and the sampler
handed PBR Water.

- **Sheets/plateaus over rocks and banks.** Cells under a rock with no water near were filled with the _mean_ nearby
  vanilla level (mixes in a higher pool) or the mesh cut's capped top (surface + 64) less 35, i.e. ~40 cm over the pool:
  a flat sheet over every low rock. Now the _lowest_ nearby vanilla level, so it always hides inside the rock.
- **Falls tent.** Since the surface became absolute, a plane reaching across a vanilla level step followed the other
  pool's water up or down the step (clamped at 2 m): a tall sheet. The spline now reads cells more than 48-80 units from
  the drawn plane as the plane itself, so each plane follows only its own pool's water.
- **Vortex funnels.** Vorticity confinement fed every eddy into a funnel. Removed; Turbulence now only scales the stir.
- **Lid over low rock flanks (the main one).** Mesh-cut solid points had the cap (water + 64) as their bed, so the output's
  "hold a dry cell's level under its bed" did nothing there and the pool's level ran on as a glassy lid over every gently
  sloped flank. Solid points now take the mesh's real skin (ComposeObstacleBlock, SOLID_SKIN 1). A/B sim on/off at the
  PBR 2 rock: lid gone. Left: a glassy rise on the rock's upstream side (likely the real pile-up, check its height).
- **Beige milky patches / pale flaps.** Our own foam layer drawn over his water (FoamStrength 0 removes them; the water's
  shape then reads well). Not ported into his foam renderer yet.
- Probe (PBR 2): falls current gives the pool's +5-8 cm and most of its speed (falls off: 0.5 m/s, -2 cm); CurrentPull
  0.1 changes little. RiverSpeed 0 (pond driven by falls alone) is wild: 2+ m/s, 20-29% at the energy limit.
- Victor: the lip on the rocks' upstream side is fine (a precursor to water running over them).

**Next session:** (1) Victor to decide: stop drawing our foam on PBR water until the port (recommended); (2) judge motion
(downstream flow vs bobbing) in game; (3) the falls-only pond case (MixFallCurrents noRiver path); (4) remove the energy
cap and swirl dip bound if nothing hits them now; (5) the earlier later-list (ripple sources, foam port, buoyancy, lakes,
tell Davo the flowmap sign).

## Polish rounds 2026-11-14 (in game, PBR 1-4 + probe)

Each fixed in the sim or as data preparation, never as a display effect:

1.  Films on rock tops drained fast unless running (FILM_DRAIN, SHEET_SPEED); under 4 cm not shown (SHEET_MIN_DEPTH).
2.  Gravity on the deviation from vanilla's level between vanilla-wet cells (option A, see review section).
3.  River current scaled by 0.5 m over vanilla depth (0.25x-1.5x): pool 2.3-2.9 -> 1.1-1.9 m/s.
4.  Sheets only on beds under slope 0.6 (`SHEET_MAX_BED_SLOPE`): one height per cell cannot hold water on a flank.
5.  Energy limit in the sim (`WaterForcesCS`): within two cells of a wall or on a bed above vanilla, the surface stands at
    most vanilla's level + v^2/2g of the fastest water around + 5 cm. Output pass carries only the pool's level into
    dry cells. Probe: pool cells near rocks over 0.45 m 20-30% -> 1-5%; water on rocks PBR 2 151 cells / 1.12 m max ->
    44 / 0.55 m, PBR 3 515 / 1.63 m -> 106 / 0.89 m. PBR 2 looks clean.

**Open, next session:**

- PBR 4: jagged translucent fringe on the near rock's falls-facing edge; likely the falls' jet (~2.6 m/s, climb
  ~0.4 m allowed) throwing water on its top edge. Test with Falls Push 0, then tame the falls' push/reach (step 5).
- PBR 3: one small shard on the near rock's right.
- His water draws up to 16 ms worst: measure Adaptive Tessellation on vs off.
- Not yet checked: waves travelling downstream, doubled surface at vanilla plane steps, motion smear.

## History (2026-11-13)

- Merge 51f0564a9; falls keep our shading, never tessellated.
- Rounds stacking display terms on the sim (local mean, upwelling, standing waves, relief, wall and sheet rules) were
  tried at PBR 1-4 (`C:/dev/cs-deploy/tools/test_cams.json`, shots in `C:/dev/cs-deploy/tools/shots/pbrset`). Kept from
  them: no rest-current push into walls, wet beds above vanilla drain slowly (water can climb), wake energy loss behind
  obstacles, bed rays at 0.5 m. The probe then measured the sim within ~10 cm of vanilla's level near rocks on
  average, with the remaining trouble in fast stretches (up to 6 m/s) and the falls' jet.
