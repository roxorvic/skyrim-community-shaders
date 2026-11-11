#pragma once

#include "SolverCommon.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace FluidFieldSim
{
	/**
	 * @brief D21: what moving actors do to the fog near the player (D194).
	 *
	 * Pushers: within the contact radius each moving foot, calf and hip (whole body for creatures without a humanoid
	 * skeleton) shoves the fog; farther out each moving actor is one. Contacts: within the contact radius every actor's legs
	 * as capsules, moving or not, for the contact map's exact cuts: a humanoid's by name, a creature's by any bone named as a
	 * leg's part (its whole body where it has none). Nearest actors first, up to the limit. Main thread.
	 */
	class AirSources
	{
	public:
		/**
		 * @brief Builds this step's pushers and contacts, world units.
		 * @param a_contactReach Distance from the player within which actors move by their skeleton and cut by their legs.
		 * @param a_outerReach Half-width round the player beyond which actors are ignored.
		 * @param a_strength Scales how hard actors push the fog (0: not at all).
		 * @param a_shove World units per second a running actor shoves the fog aside.
		 * @param a_maxMovers Most pushers, and most contacts, to return.
		 */
		void Update(const RE::NiPoint3& a_playerPos, float a_contactReach, float a_outerReach, float a_deltaTime, float a_strength, float a_shove, uint32_t a_maxMovers);

		const std::vector<Mover>& GetPushers() const { return pushers; }
		const std::vector<Mover>& GetContacts() const { return contacts; }

		/** @brief Forgets every actor's history, after a reset or teleport. */
		void Clear();

	private:
		struct History
		{
			RE::NiPoint3 position;
			uint32_t lastSeen = 0;  ///< Update counter when last seen
		};

		/** @brief A creature's leg bones, each with its parent: capsules for the contact map. Valid while root is its 3D. */
		struct Legs
		{
			const RE::NiAVObject* root = nullptr;
			std::vector<std::pair<const RE::NiAVObject*, const RE::NiAVObject*>> bones;
			uint32_t lastSeen = 0;
		};
		/** @brief The leg bones of an actor's skeleton, found once per 3D. */
		const Legs& FindLegs(RE::Actor& a_actor, RE::NiAVObject& a_root);

		/** @brief Updates between sightings after which an actor's history is dropped. */
		static constexpr uint32_t FORGET_AFTER = 120;

		std::unordered_map<uint64_t, History> history;  ///< Keyed by actor form ID (high bits) and body part (low byte)
		std::unordered_map<uint32_t, Legs> legs;        ///< Keyed by actor form ID
		std::vector<Mover> pushers;
		std::vector<Mover> contacts;
		uint32_t updateCounter = 0;
	};
}
