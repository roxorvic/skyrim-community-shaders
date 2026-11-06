# Runtime falls: each vanilla falls mesh culled, remeshed and baked on its own

Nov 6, 2026 · agreed with @Victor

The unified offline sheets (one merged mesh per site, built by tools/unify) are parked. The falls go back to the first approach: every vanilla falls mesh is kept as its own piece, culled to what can be seen and remeshed at runtime; only bake data is made offline. Nothing in this plan ships meshes.

## Decisions

- **Which falls:** the big sheets (FallSites' Body class). The thin streak falls (FXWaterfallThin) stay fully vanilla.
- **Cull, at runtime, per mesh:**
    - under the landscape (D172, against the landscape height cache);
    - inside static rock (the statics' render triangles through a CPU BVH; Havok cannot run on a worker, D172);
    - hidden from outside, sheet inside sheet included (D173's outer-shell trim: rays over the sphere, a 64-unit margin, triangles split along the cut).
- **Remesh, at runtime:** the kept part at a modest, even density (about 48-64 units an edge, to be measured), its outer edge and cut lines kept as the boundary. Much lighter than the old dense copies (D187).
- **Budget:** all of it on workers. No frame over about 20 ms from the falls, settled within a few seconds of a cell loading, nearest falls first. Results cached in memory and on disk, so travelling back is instant and stays consistent across the map.
- **Bakes, offline for now:** one per vanilla falls mesh, keyed by its reference (plugin, form ID, model). Baked against the vanilla mesh's own triangles, so they depend only on vanilla data: at runtime each remeshed vertex reads the bake at its point on the nearest vanilla triangle. The harness does not have to reproduce the runtime cull or remesh. Size and compression come later.
- **Linking:** speed and aeration stay consistent between the meshes of one fall. At runtime each mesh's exit edge hands its values to the downstream meshes it meets, so neighbours agree at the seam.
- **No bake file** (a mod-added fall, say): still culled and remeshed, drawn with the falls shading and default speed and aeration. Once bakes can run at startup or runtime, these get baked too.
- **The unified pipeline is parked,** not deleted: tools/unify, the site caches and siteA.unfs stay, switched off.

## Steps

Each step ends in a build checked in game (the falls saves, clear weather, screenshots at the same spots; PresentMon for anything that adds per-frame work) and a decision record.

1.  **The per-mesh path back.** Restore from before cb407070b the per-mesh falls (each vanilla mesh drawn by the river's falls shader), the landscape cut (D172), the outer-shell trim (FallVisibility, D173) and the worker scheduling and caches (D172, D185). Park the unified site path behind it. No bakes yet: default speed and aeration.
2.  **Rock cut.** The statics' render triangles in a BVH; triangles inside rock cut as the landscape cut does.
3.  **Remesh.** The kept surface remeshed at an even density; measure triangle count and shading quality on site A against the vanilla density.
4.  **Bake per mesh, offline.** The harness bakes each vanilla mesh from a dump of the game's data (its triangles, the rock and the water round it) into a file keyed by its reference; the game loads it and maps it onto the remeshed vertices.
5.  **Linking.** Exit edges to entry edges between a fall's meshes; speed and aeration carried downstream.
6.  **Travel.** The disk cache of culled and remeshed meshes, streaming as cells load; timed cold and warm (as D185).

## Left for later

- Bakes at startup or runtime, which brings mod-added falls in.
- Compression of the bake files.
