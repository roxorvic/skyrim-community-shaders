#pragma once

#include "SolverCommon.h"

#include <cstdint>
#include <deque>
#include <mutex>
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
	 *
	 * D194 step 6, close by: drawn weapons cut along the blade and push along its swing; dead bodies cut by their torso.
	 * Anywhere in the grid: projectiles push along their flight and cut their path; explosions (an Update hook) shove
	 * outward and cut; shouts (spell-cast events) push a cone ahead of the shouter; fires clear a circle round them.
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
		void Update(const RE::NiPoint3& a_playerPos, float a_contactReach, float a_outerReach, float a_deltaTime, float a_strength, float a_shove, uint32_t a_maxMovers,
			const std::vector<HeatSource>& a_heat);

		const std::vector<Mover>& GetPushers() const { return pushers; }
		const std::vector<Mover>& GetContacts() const { return contacts; }

		/** @brief Forgets every actor's history, after a reset or teleport. */
		void Clear();

		/** @brief Hooks explosions' updates and listens for shouts, once (after the game's data loads). */
		static void Install();

	private:
		struct History
		{
			RE::NiPoint3 position;
			uint32_t lastSeen = 0;  ///< Update counter when last seen
		};

		/** @brief An actor's bones the fog reads, found by name once per 3D root and held (NiPointer: they can't dangle);
		 * looked up again when the root changes. Creature legs (each with its parent) are found on first use. */
		struct Bones
		{
			RE::NiPointer<RE::NiAVObject> root;
			std::array<RE::NiPointer<RE::NiAVObject>, 5> parts;                ///< HUMANOID_PARTS
			std::array<std::array<RE::NiPointer<RE::NiAVObject>, 3>, 2> legs;  ///< LEGS
			std::array<RE::NiPointer<RE::NiAVObject>, 2> attach;               ///< WEAPON, SHIELD
			std::vector<std::pair<RE::NiPointer<RE::NiAVObject>, RE::NiPointer<RE::NiAVObject>>> creatureLegs;
			bool creatureLegsFound = false;
			uint32_t lastSeen = 0;
		};
		/** @brief The actor's cached bones, found again when its 3D root changed. */
		Bones& BonesOf(RE::Actor& a_actor, RE::NiAVObject& a_root);

		/** @brief Updates between sightings after which an actor's history is dropped. */
		static constexpr uint32_t FORGET_AFTER = 120;

		std::unordered_map<uint64_t, History> history;  ///< Keyed by actor form ID (high bits) and body part (low byte)
		std::unordered_map<uint32_t, Bones> bones;      ///< Keyed by actor form ID
		std::vector<Mover> pushers;
		std::vector<Mover> contacts;
		/** @brief Each projectile's position at the last update: its real motion (a stuck arrow keeps its flight velocity). */
		std::unordered_map<uint32_t, std::pair<RE::NiPoint3, uint32_t>> projectiles;
		/** @brief Shouts still pushing: the shouter and seconds left. */
		std::vector<std::pair<RE::ObjectRefHandle, float>> shouts;
		uint32_t updateCounter = 0;
	};
}
