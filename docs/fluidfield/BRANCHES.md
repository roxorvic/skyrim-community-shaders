# Fluid Field branches and worktrees

Since 2026-11-13: one shared base, two feature lines, each in its own worktree. Every branch holds all the code (builds keep
working, merges stay clean); the split is where work happens.

| Branch               | Worktree                               | What changes there                                           |
| -------------------- | -------------------------------------- | ------------------------------------------------------------ |
| `fluid-field/base`   | `C:/dev/skyrim-community-shaders-base` | The shared base, and nothing feature-specific                |
| `fluid-field/water`  | `C:/dev/skyrim-community-shaders`      | Waterfalls and Shallow Water (coupled: falls feed the river) |
| `fluid-field/fog-v3` | `C:/dev/skyrim-community-shaders-fog`  | Ground Fog                                                   |

**The shared base:** `src/Features/FluidField/Field.*`, `LandscapeCache.*`, `WaterSurfaces.*`, `GpuSpans.*`,
`SolverCommon.h`, `HiddenObjects.*`, `EngineGrid.h`, and the Fluid Field hook points in `State.cpp`, `Globals.*`,
`Feature.cpp`, `FeatureBuffer.cpp`. Also these docs' shared parts.

**Rules:**

- A change to the shared base is made on `fluid-field/base`, committed there, then merged into both feature branches.
- Feature work touches only its own files: Ground Fog (`FluidFieldGroundFog.*`, `FogSolver.*`, `GroundTiles.*`,
  `AirSources.*`, `LandscapeDepth.*`, `features/Ground Fog/`); water (`FluidFieldWaterfalls.*`, `FluidFieldShallowWater.*`,
  `Fall*`, `River*`, `Bed*`, `MeshCut.*`, `Wake*`, `WaterMap*`, `Waterfall*`, `VanillaFoam.*`, `ShallowWaterSolver.*`,
  `features/Waterfalls/`, `features/Shallow Water/`).
- A feature change that turns out to need the base: move the base part to `fluid-field/base` first.
- One staging folder (`C:/dev/cs-deploy`) and one game: one worktree's build is tested at a time. `launch.sh` says which
  worktree the staged DLL came from, and stops if it is not `REPO`'s build.
