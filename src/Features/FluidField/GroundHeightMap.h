#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	class LandscapeCache;
	class WaterSurfaces;

	/**
	 * @brief D193: the triangles of the statics round the player that shape the ground fog, world space, bucketed by area.
	 *
	 * By model folder: plants, clutter, effects and markers are left out; rocks, logs, trees and stumps are walls (every
	 * triangle); everything else (roads, dirt cliffs, mountains, buildings) by each triangle's facing: up is ground (only within
	 * GROUND_WINDOW over the landscape: roofs and canopies are not), down an underside (an overhang), steep a wall. References
	 * are found every so often and read a few per frame. Main thread.
	 */
	class GroundMeshes
	{
	public:
		enum class Kind : uint8_t
		{
			Ground,
			Underside,
			Wall
		};

		struct Triangle
		{
			float a[3], b[3], c[3];
			Kind kind;
		};

		/** @brief A world rectangle whose triangles changed (references read in). */
		struct Rect
		{
			float min[2], max[2];
		};

		/** @brief Finds and reads references round a_centre out to a_radius, within the frame's budget. */
		void Update(const LandscapeCache& a_landscape, float a_centreX, float a_centreY, float a_radius);
		void Clear();

		/** @brief Calls a_visit for every triangle whose bucket touches the rectangle (a triangle may come more than once). */
		template <class Visit>
		void ForEach(const float a_min[2], const float a_max[2], Visit&& a_visit) const
		{
			const auto x0 = static_cast<int32_t>(std::floor(a_min[0] / BUCKET)), x1 = static_cast<int32_t>(std::floor(a_max[0] / BUCKET));
			const auto y0 = static_cast<int32_t>(std::floor(a_min[1] / BUCKET)), y1 = static_cast<int32_t>(std::floor(a_max[1] / BUCKET));
			for (int32_t y = y0; y <= y1; y++) {
				for (int32_t x = x0; x <= x1; x++) {
					if (const auto it = buckets.find(Key(x, y)); it != buckets.end())
						for (const auto& triangle : it->second)
							a_visit(triangle);
				}
			}
		}

		/** @brief Rectangles changed since a_version (a consumer's last), and the version now. */
		uint64_t GetVersion() const { return static_cast<uint64_t>(changes.size()) + dropped; }
		void ChangesSince(uint64_t a_version, std::vector<Rect>& o_rects) const;

	private:
		static constexpr float BUCKET = 512.0f;
		/** @brief World units over the landscape an upward triangle may lie and still be ground (porches, docks, not roofs). */
		static constexpr float GROUND_WINDOW = 300.0f;
		static constexpr double READ_BUDGET_MS = 2.0;
		/** @brief Frames between looks for new references. */
		static constexpr uint32_t SCAN_INTERVAL = 90;

		static uint64_t Key(int32_t a_x, int32_t a_y) { return (static_cast<uint64_t>(static_cast<uint32_t>(a_x)) << 32) | static_cast<uint32_t>(a_y); }
		/** @brief Reads one reference's triangles into the buckets. */
		void Read(RE::TESObjectREFR& a_ref, const LandscapeCache& a_landscape);

		std::unordered_map<uint64_t, std::vector<Triangle>> buckets;
		std::unordered_map<uint32_t, std::pair<float, float>> seen;  ///< Form IDs read (or skipped), and where (dropped with the buckets)
		std::deque<RE::ObjectRefHandle> queue;                       ///< Found, still to read
		std::vector<Rect> changes;                                   ///< Every rectangle read in, in order (ChangesSince)
		uint64_t dropped = 0;                                        ///< Changes forgotten from the front of the log
		uint32_t framesToScan = 0;
	};

	/**
	 * @brief D21: ground height under the wind grid for the ground fog. D193: x the ground (the landscape, or a ground mesh
	 * standing on it: a road, a ledge, a floor), y the top of a wall standing there (a rock, a log, a trunk, a cliff's face),
	 * z the floor under an overhang (the fog lies on both levels), w the ground's largest rise to a neighbour; all rasterized
	 * from GroundMeshes' triangles, so the map is whole as soon as the landscape and the meshes are in.
	 *
	 * Toroidal like the field (world cell c in texel c & (res - 1)); on scroll only new cells are filled, and cells over
	 * unloaded land are retried. Main thread; the texture is updated on the render thread's context.
	 */
	class GroundHeightMap
	{
	public:
		/** @brief Height written where the landscape is not known (no fog forms there). */
		static constexpr float NO_GROUND = -1.0e7f;
		/** @brief Written over water while fog on water is off (lower than NO_GROUND: the fog pass draws none there). */
		static constexpr float ON_WATER = -2.0e7f;
		/** @brief Fog lies on water surfaces (off for now: rivers' fog comes back with placement by water speed). */
		static constexpr bool FOG_ON_WATER = false;

		/**
		 * @brief Brings the map up to date with the grid's placement and the meshes, and uploads what changed.
		 * @return False if the texture could not be created.
		 */
		bool Update(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const GroundMeshes& a_meshes,
			const int32_t a_gridMin[2], uint32_t a_resolution, float a_cellSize);

		void Release();

		ID3D11ShaderResourceView* GetSRV() const { return srv.get(); }
		/** @brief Highest known ground, so the fog pass can skip rays above the fog. */
		float GetMaxHeight() const { return maxHeight; }

	private:
		/** @brief Updates between retries of cells over unloaded land. */
		static constexpr uint32_t RETRY_INTERVAL = 60;
		/** @brief An overhang's underside clears the floor beneath it by at least this much. */
		static constexpr float OVERHANG_CLEARANCE = 40.0f;
		/** @brief Rows rasterized at a time, and milliseconds of it per update. */
		static constexpr int32_t BAND_ROWS = 32;
		static constexpr double RASTER_BUDGET_MS = 2.0;

		/** @brief A rectangle of world cells (inclusive) still to rasterize. */
		struct CellRect
		{
			int32_t min[2], max[2];
		};

		/** @brief Queues a rectangle of world cells, clipped to the grid, in bands of BAND_ROWS rows. */
		void Queue(int32_t a_minX, int32_t a_minY, int32_t a_maxX, int32_t a_maxY, const int32_t a_gridMin[2]);
		/** @brief Rasterizes the meshes over one rectangle of world cells (in the grid) from the landscape up. */
		void Rasterize(const CellRect& a_rect, const GroundMeshes& a_meshes);
		/** @brief Marks a texel's row, and its neighbours' (their steepness), for the next upload. */
		void MarkDirty(std::size_t a_index);

		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		std::vector<float> heights;  ///< Toroidal, as the texture
		std::vector<float> objects;  ///< Top of a wall standing there, or NO_GROUND
		std::vector<float> lowers;   ///< The floor under an overhang there, or NO_GROUND
		std::vector<float> land;     ///< The landscape's own height per texel; ON_WATER or NO_GROUND where no fog lies
		std::vector<float> upload;
		std::vector<float> scratch;  ///< Per texel of a rectangle: the highest underside under its ground
		std::vector<uint8_t> rowDirty;
		std::deque<CellRect> rasterQueue;
		uint64_t meshVersion = 0;  ///< GroundMeshes' version this map has taken in
		uint32_t resolution = 0;
		float cellSize = 0.0f;
		int32_t filledMin[2]{};  ///< Grid placement the map was last filled for
		bool filled = false;
		uint32_t updatesUntilRetry = 0;
		float maxHeight = NO_GROUND;
	};
}
