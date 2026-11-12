#pragma once

// Free of plugin headers (no PCH, globals or CommonLib) so tests/FluidField can run the solvers on a WARP device.

#include <d3d11.h>

#include <cstdint>
#include <functional>

namespace FluidFieldSim
{
	/**
	 * @brief D193: something moving through the fog, matching `Mover` in FogCommon.hlsli: a segment (a point when both ends
	 * match) with a Gaussian falloff.
	 * World units throughout.
	 */
	struct Mover
	{
		float from[2];      ///< Segment start, world xy
		float to[2];        ///< Segment end, world xy
		float radius;       ///< Gaussian falloff radius
		float shove;        ///< World units per second it shoves the fog aside at its edge
		float velocity[2];  ///< Push, world units per second
		float cut;          ///< Share of the fog it clears outright where it passes (0 to 1)
		float bottom, top;  ///< The heights it spans, world units
		float pad;
	};
	static_assert(sizeof(Mover) == 48);

	/** @brief A fire the fog clears round (its heat dries the air): where, and how far round. */
	struct HeatSource
	{
		float x, y, z, radius;
	};

	/** @brief A solver's grid placement and step. */
	struct StepParams
	{
		std::int32_t gridMin[2]{};   ///< World cell of logical texel (0, 0)
		float cellSize = 16.0f;      ///< World units per texel
		float deltaTime = 0.0f;      ///< Seconds to simulate
		float velocityDecay = 1.0f;  ///< Unused by the shallow water solver
		std::uint32_t jacobiIterations = 0;
	};

	/** @brief Names a D3D11 object for RenderDoc. */
	using NameFn = std::function<void(ID3D11DeviceChild*, const char*)>;
	/** @brief Compiles a compute shader from a file under the FluidField shader folder; returns an owned reference or null. */
	using CompileFn = std::function<ID3D11ComputeShader*(const wchar_t* fileName)>;
}
