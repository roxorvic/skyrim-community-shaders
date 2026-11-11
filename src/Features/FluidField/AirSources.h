#pragma once

#include "WindSolver.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief D21: wind and fog movers from moving actors near the player.
	 *
	 * Inside the fine level (D193) it follows the body, not the collision capsule: each moving foot, calf and hip (whole
	 * body for creatures without a humanoid skeleton) drags the air and ploughs the fog; farther out each actor is one
	 * mover. Nearest actors first, up to the limit. Main thread.
	 */
	class AirSources
	{
	public:
		/**
		 * @brief Builds this step's movers, world units.
		 * @param a_fineReach Half-width round the player within which actors move by their skeleton (0: none do).
		 * @param a_outerReach Half-width round the player beyond which actors are ignored.
		 * @param a_strength Scales how hard actors push the air (0: not at all).
		 * @param a_shove World units per second a running actor shoves the fog aside.
		 * @param a_maxMovers Most movers to return.
		 */
		const std::vector<Mover>& Update(const RE::NiPoint3& a_playerPos, float a_fineReach, float a_outerReach, float a_deltaTime, float a_strength,
			float a_shove, uint32_t a_maxMovers);

		/** @brief Whether anything moved within the fine reach in the last update. */
		bool MovedNearby() const { return movedNearby; }

		/** @brief Forgets every actor's history, after a reset or teleport. */
		void Clear();

	private:
		struct History
		{
			RE::NiPoint3 position;
			uint32_t lastSeen = 0;  ///< Update counter when last seen
		};

		/** @brief Updates between sightings after which an actor's history is dropped. */
		static constexpr uint32_t FORGET_AFTER = 120;

		std::unordered_map<uint64_t, History> history;  ///< Keyed by actor form ID (high bits) and body part (low byte)
		std::vector<Mover> movers;
		uint32_t updateCounter = 0;
		bool movedNearby = false;
	};
}
