#pragma once

#include "LandscapeCache.h"
#include "SolverCommon.h"
#include "WaterSurfaces.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief D193: the triangles of the statics round the player that shape the ground fog, world space, bucketed by area.
	 *
	 * By model folder: plants, clutter, effects and markers are left out; rocks, logs, trees, stumps and mountains are rock
	 * (every triangle); everything else by each triangle's facing: up is ground (within GROUND_WINDOW over the landscape, so
	 * roofs and canopies are not; `Landscape\` meshes at any height), down is dropped (one level per column, D194), steep a
	 * wall. References are found every so often and read a few per frame. Main thread.
	 */
	class GroundMeshes
	{
	public:
		enum class Kind : uint8_t
		{
			Ground,
			Wall,
			Underside  ///< An overhang's: the floor under it is a second level (D195)
		};

		struct Triangle
		{
			float a[3], b[3], c[3];
			Kind kind;
			bool steep = false;  ///< Ground too steep to carry fog: it sets the height only
			bool flat = false;   ///< A rock's face turned up: ground when it lies under an overhang (a mine entrance's floor)
			bool lean = false;   ///< An underside facing down only a little: one only under a thin top (a lip)
		};

		/** @brief A world rectangle whose triangles changed (references read in). */
		struct Rect
		{
			float min[2], max[2];
		};

		/**
		 * @brief Finds and reads references round a_centre out to a_radius, within the frame's budget. a_skip says a
		 * reference's bounds are already known (its tiles cached), so it is not read.
		 */
		void Update(const LandscapeCache& a_landscape, float a_centreX, float a_centreY, float a_radius, double a_budgetMs, const std::function<bool(const Rect&)>& a_skip);
		void Clear();

		/** @brief Appends every triangle whose bucket touches the rectangle (a triangle may come more than once). */
		void Copy(const float a_min[2], const float a_max[2], std::vector<Triangle>& o_triangles) const;

		/** @brief Rectangles changed since a_version (a consumer's last), and the version now. */
		uint64_t GetVersion() const { return static_cast<uint64_t>(changes.size()) + dropped; }
		void ChangesSince(uint64_t a_version, std::vector<Rect>& o_rects) const;

		/** @brief Scans done since the last Clear; a rectangle is settled once a later scan covered it and nothing is left to read there. */
		uint32_t GetScanCount() const { return scanCount; }
		/** @brief Fires round the player at the last scan. */
		const std::vector<HeatSource>& GetHeat() const { return heat; }
		bool Settled(const Rect& a_rect, uint32_t a_afterScan) const;

	private:
		static constexpr float BUCKET = 512.0f;
		/** @brief World units over the landscape an upward triangle may lie and still be ground (porches, docks, not roofs). */
		static constexpr float GROUND_WINDOW = 300.0f;
		/** @brief Frames between looks for new references. */
		static constexpr uint32_t SCAN_INTERVAL = 90;
		/** @brief How far a reference's triangles may reach from its position before it is read (its bounds unknown). */
		static constexpr float PENDING_REACH = 1024.0f;

		struct Pending
		{
			RE::ObjectRefHandle handle;
			float x, y;
		};

		static uint64_t Key(int32_t a_x, int32_t a_y) { return (static_cast<uint64_t>(static_cast<uint32_t>(a_x)) << 32) | static_cast<uint32_t>(a_y); }
		/** @brief Reads one reference's triangles into the buckets. */
		void Read(RE::TESObjectREFR& a_ref, const LandscapeCache& a_landscape);

		std::unordered_map<uint64_t, std::vector<Triangle>> buckets;
		std::unordered_map<uint32_t, std::pair<float, float>> seen;  ///< Form IDs found, and where (dropped with the buckets)
		std::vector<HeatSource> heat;
		std::size_t loggedHeat = SIZE_MAX;
		std::deque<Pending> queue;                                                    ///< Found, still to read (or waiting for their 3D)
		std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> waiting;  ///< Since when a reference waits for its 3D
		std::vector<Rect> changes;                                                    ///< Every rectangle read in, in order (ChangesSince)
		uint64_t dropped = 0;                                                         ///< Changes forgotten from the front of the log
		uint32_t framesToScan = 0;
		uint32_t scanCount = 0;
		float scanCentre[2]{}, scanRadius = 0.0f;
	};

	/**
	 * @brief D194: the ground the fog lies on, in two tiers (the distant fog uses LandscapeDepth). Near: tiles of 1,024 world
	 * units (128² texels of 8 u) built from the landscape and GroundMeshes on a worker thread, cached on disk once settled,
	 * kept in one toroidal texture round the player (height and flags of the top surface and of the floor under an overhang,
	 * D195). Middle: the loaded landscape at LAND_TEXEL, where a tile is not built yet. Main thread: copies inputs, uploads
	 * finished tiles.
	 */
	class GroundTiles
	{
	public:
		static constexpr uint32_t TILE_TEXELS = 128;
		static constexpr float TEXEL = 8.0f;
		static constexpr float TILE_UNITS = TILE_TEXELS * TEXEL;
		/** @brief Resident tiles per side (the texture is SLOTS * TILE_TEXELS texels across). */
		static constexpr int32_t SLOTS = 16;
		static constexpr uint32_t RESIDENT_TEXELS = SLOTS * TILE_TEXELS;
		static constexpr float NO_GROUND = -1.0e7f;

		/** @brief Flags (texture channel y), as GroundDebugCS reads them. */
		enum Flag : uint32_t
		{
			FOG_MAY_FORM = 1,
			ROCK = 2,
			STEEP = 4,
			WATER = 8,
			PROVISIONAL = 16  ///< Built before its area settled; rebuilt later
		};

		/** @brief A resident slot's tile as uploaded (consumers treat a slot whose tile is not the wanted one as unknown). */
		struct SlotTile
		{
			int32_t x, y, valid, pad;
		};

		GroundTiles() = default;
		~GroundTiles();
		GroundTiles(const GroundTiles&) = delete;
		GroundTiles& operator=(const GroundTiles&) = delete;

		/** @brief Places the window round the player, hands the worker its jobs and uploads what it finished. Render thread. */
		void Update(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, float a_playerX, float a_playerY, float a_deltaTime);
		/** @brief Drops everything (a load, another worldspace); the cache on disk stays. */
		void Clear();
		/** @brief Drops everything and stops the worker. */
		void Release();

		ID3D11ShaderResourceView* GetSRV() const { return srv.get(); }
		/** @brief Tile coordinates of the window's south-west tile. */
		const int32_t* GetTileMin() const { return tileMin; }
		const std::array<SlotTile, SLOTS * SLOTS>& GetSlots() const { return slots; }
		const std::vector<HeatSource>& GetHeat() const { return meshes.GetHeat(); }
		/** @brief The middle tier: the loaded landscape cells round the player at LAND_TEXEL (height, flags), toroidal (world
		 * texel t in texel t & (LAND_TEXELS - 1)); valid within LAND_REACH cells of GetLandCell(). */
		ID3D11ShaderResourceView* GetLandSRV() const { return landSRV.get(); }
		const int32_t* GetLandCell() const { return landCell; }
		static constexpr float LAND_TEXEL = 32.0f;
		static constexpr uint32_t LAND_TEXELS = 1024;
		static constexpr int32_t LAND_REACH = 2;

		/** @brief Tiles waiting for the worker, and the tiles kept provisional (for the UI). */
		std::size_t GetQueued() const;
		uint32_t GetProvisional() const;

	private:
		/** @brief What the worker needs for one tile: a copy of the inputs over it, so it never touches the game. */
		struct Job
		{
			int32_t x, y;
			bool loadOnly;  ///< Read it from the disk cache; reply with a miss if it is not there
			bool settled;   ///< Its area is settled: cache what is built
			LandscapeCache landscape;
			std::vector<WaterSurfaces::Plane> water;
			std::vector<GroundMeshes::Triangle> triangles;
		};
		struct Result
		{
			int32_t x, y;
			bool miss;                  ///< loadOnly found nothing on disk
			bool settled;               ///< From the cache, or built settled
			std::vector<float> texels;  ///< TILE_TEXELS² (top height, flags, lower height, flags)
		};
		/** @brief A resident slot's state on the main thread. */
		struct Slot
		{
			int32_t x = INT32_MIN, y = INT32_MIN;  ///< The tile wanted here
			bool inFlight = false;
			bool missed = false;             ///< The disk cache had none: build it
			bool built = false;              ///< Uploaded at least once
			bool final = false;              ///< Uploaded settled (from the cache or built settled)
			bool dirty = false;              ///< Its inputs changed since its last build
			bool settled = false;            ///< Its area is settled (GroundMeshes::Settled, landscape all there)
			uint32_t landScan = UINT32_MAX;  ///< GroundMeshes' scan count when its landscape was first all there
			double lastBuild = -1e9;
			double nextCheck = 0.0;
		};

		static uint64_t Key(int32_t a_x, int32_t a_y) { return (static_cast<uint64_t>(static_cast<uint32_t>(a_x)) << 32) | static_cast<uint32_t>(a_y); }
		static Slot& SlotOf(std::array<Slot, SLOTS * SLOTS>& a_slots, int32_t a_x, int32_t a_y) { return a_slots[static_cast<std::size_t>(a_y & (SLOTS - 1)) * SLOTS + (a_x & (SLOTS - 1))]; }

		/** @brief Worker: runs jobs nearest the player first until stopped. */
		void Run(std::stop_token a_stop);
		/** @brief Worker: rasterizes one tile's height and flags from its inputs. */
		static void Build(const Job& a_job, std::vector<float>& o_texels);
		/** @brief Starts the cache folder for a worldspace. */
		void EnterWorldspace(const std::string& a_name);

		GroundMeshes meshes;
		uint64_t meshVersion = 0;

		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		winrt::com_ptr<ID3D11Texture2D> landTexture;
		winrt::com_ptr<ID3D11ShaderResourceView> landSRV;
		std::unordered_set<uint64_t> landFilled;  ///< Cells rasterized into the land map
		int32_t landCell[2]{};
		/** @brief Rasterizes one loaded cell a frame into the land map (the middle tier). */
		void UpdateLand(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, float a_playerX, float a_playerY);
		int32_t tileMin[2]{};
		std::array<Slot, SLOTS * SLOTS> state{};
		std::array<SlotTile, SLOTS * SLOTS> slots{};
		std::deque<Result> uploads;           ///< Finished, waiting for their frame's upload budget
		std::unordered_set<uint64_t> cached;  ///< Tiles known settled on disk: their references need no reading
		std::size_t landCells = 0;
		uint64_t waterSignature = 0;

		std::string worldspace;
		double clock = 0.0;                               ///< Seconds of updates
		std::chrono::steady_clock::time_point enteredAt;  ///< Last Clear: reads run on a bigger budget just after

		// Shared with the worker
		mutable std::mutex mutex;
		std::condition_variable_any wake;
		std::vector<std::unique_ptr<Job>> jobs;
		std::vector<Result> results;
		float playerPos[2]{}, travel[2]{};
		std::filesystem::path workerCacheDir;  ///< This worldspace and load order's cache folder
		uint64_t generation = 0;               ///< Bumped by Clear; the worker drops results from older generations
		std::jthread worker;
	};
}
