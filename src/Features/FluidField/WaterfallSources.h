#pragma once

#include "ShallowWaterSolver.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	class WaterSurfaces;

	/**
	 * @brief D22: waterfalls that drive the simulated water where they land.
	 *
	 * Each loaded body mesh's (FXWaterfallBody*, FXWaterfallThin*) shape is read once from its geometry (bounds as fallback).
	 * Bodies landing on water merge into falls; each emits a downstream current and foam where it meets the water. Bodies
	 * starting at a water's surface are its brinks, where it drains. Main thread.
	 */
	class WaterfallSources
	{
	public:
		/** @brief A waterfall landing on water. */
		struct Fall
		{
			float plunge[3]{};     ///< Where it meets the water, world units (z is the water surface)
			float width = 0.0f;    ///< Width of the falling water at the plunge, world units
			float direction[2]{};  ///< Horizontal direction it falls toward (unit length), or zero
		};

		/** @brief Rescans the loaded waterfalls around a point every few updates. */
		void Update(const WaterSurfaces& a_water, float a_centreX, float a_centreY, float a_halfSize);

		/**
		 * @brief This step's emitters for one water level, in its logical texel coordinates.
		 * @param a_strength Scales every effect (0: none).
		 */
		void AppendEmitters(std::vector<WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes, float a_strength,
			uint32_t a_maxEmitters, bool a_foam) const;

		/** @brief The landings' and brinks' currents for the water's rest flow (WaterParams::fallCurrents). */
		void AppendCurrents(std::vector<WaterFallCurrent>& a_currents, float a_strength) const;

		void Clear();

		const std::vector<Fall>& GetFalls() const { return falls; }

	private:
		/** @brief A body's shape, read once while its 3D is loaded. */
		struct Body
		{
			float plunge[3]{};
			float width = 0.0f;
			float direction[2]{};
			float lowest = 0.0f;   ///< Lowest point of the body, world units
			float highest = 0.0f;  ///< Highest point
			float top[2]{};        ///< Centre of its top band, where it tips over
			float topWidth = 0.0f;
			std::vector<RE::NiPoint3> points;  ///< World-space vertices, thinned, to find where it meets the water
		};

		/** @brief Updates between scans of the loaded references. */
		static constexpr uint32_t SCAN_INTERVAL = 60;
		/** @brief Most falls kept, nearest first. */
		static constexpr std::size_t MAX_FALLS = 6;
		/** @brief Most landings, and most brinks, given currents, nearest first. */
		static constexpr std::size_t MAX_CURRENTS = 16;

		static bool ReadBody(RE::TESObjectREFR& a_ref, Body& a_body);

		std::unordered_map<uint32_t, Body> bodies;  ///< Keyed by form ID
		std::vector<Fall> falls;
		std::vector<Fall> landings;  ///< Every landing kept for the currents (falls holds the nearest few)
		std::vector<Fall> brinks;
		uint32_t updatesUntilScan = 0;
	};
}
