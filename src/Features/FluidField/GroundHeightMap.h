#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace FluidFieldSim
{
	class LandscapeCache;
	class WaterSurfaces;

	/**
	 * @brief D21: ground height under the wind grid for the ground fog: landscape, or the water surface over it, so fog
	 * lies on water as mist. D193: one ground layer: placed static meshes on the landscape (roads, rocks, ledges) replace
	 * its height where they stand on it, from downward Havok rays cast a few per frame, nearest first. Rocks, logs and trunks
	 * standing on the ground are kept apart (the second channel: their top), as walls to the fog's wind; under an overhang
	 * the floor beneath is the third (the fog lies on both levels); the fourth is the ground's largest rise to a neighbour.
	 *
	 * Toroidal like the field (world cell c in texel c & (res - 1)); on scroll only new cells are sampled, and cells over
	 * unloaded land are retried. Main thread; the texture is updated on the render thread's context.
	 */
	class GroundHeightMap
	{
	public:
		/** @brief Height written where the landscape is not known (no fog forms there). */
		static constexpr float NO_GROUND = -1.0e7f;

		/**
		 * @brief Brings the map up to date with the grid's placement and uploads what changed.
		 * @return False if the texture could not be created.
		 */
		bool Update(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const int32_t a_gridMin[2],
			uint32_t a_resolution, float a_cellSize);

		void Release();

		ID3D11ShaderResourceView* GetSRV() const { return srv.get(); }
		/** @brief Highest known ground, so the fog pass can skip rays above the fog. */
		float GetMaxHeight() const { return maxHeight; }

	private:
		/** @brief Updates between retries of cells over unloaded land. */
		static constexpr uint32_t RETRY_INTERVAL = 60;
		/** @brief World units above the landscape a ray starts (meshes on it, not roofs or canopies) and below it it ends. */
		static constexpr float RAY_ABOVE = 300.0f;
		static constexpr float RAY_BELOW = 32.0f;
		/** @brief Milliseconds of rays per update. */
		static constexpr double RAY_BUDGET_MS = 0.5;

		/** @brief Casts the queued cells' rays, nearest the grid's centre first, until the budget runs out. */
		bool CastPending(const int32_t a_gridMin[2]);

		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		std::vector<float> heights;                        ///< Toroidal, as the texture
		std::vector<float> objects;                        ///< Top of a rock, log or trunk standing there, or NO_GROUND
		std::vector<float> lowers;                         ///< The floor under an overhang there, or NO_GROUND
		std::vector<float> upload;                         ///< Ground and object interleaved, as the texture
		std::vector<float> land;                           ///< The landscape's own height per texel (water and meshes left out)
		std::vector<std::pair<int32_t, int32_t>> pending;  ///< World cells still to cast, nearest last
		bool pendingSorted = true;
		uint32_t resolution = 0;
		float cellSize = 0.0f;
		int32_t filledMin[2]{};  ///< Grid placement the map was last filled for
		bool filled = false;
		uint32_t updatesUntilRetry = 0;
		float maxHeight = NO_GROUND;
	};
}
