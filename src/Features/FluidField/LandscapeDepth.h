#pragma once

#include <d3d11.h>

namespace FluidFieldSim
{
	/**
	 * @brief D194 far tier (agreed deviation, fog-v3.md): the depth of the landscape alone, LOD landscape included. During the
	 * depth pre-pass, every landscape pass the game draws is drawn once more into a depth target of its own (the way Terrain
	 * Blending draws terrain twice, its hooks chained). Where the scene's depth matches it, the pixel shows landscape: trees,
	 * rocks and buildings stand in front of it, and water (not in the pre-pass) lies over its bed.
	 */
	class LandscapeDepth
	{
	public:
		/** @brief Chains the depth pre-pass and render-pass hooks, once (after Terrain Blending's). */
		static void Install();
		/** @brief Draws into the target only while on (the fog is on). */
		static void SetEnabled(bool a_enabled);
		/** @brief The landscape's depth this frame (as the main depth), or null. */
		static ID3D11ShaderResourceView* GetSRV();
		/** @brief Landscape passes wholly within this distance of the eye are not drawn into it (the distant fog doesn't
		 * read them there). */
		static void SetNearSkip(float a_distance);
		/** @brief How far the landscape drawn this frame reaches from the eye, world units (0 until drawn). */
		static float GetReach();
	};
}
