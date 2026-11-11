#include "AirSources.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Slower than this (world units per second) stirs nothing. */
		constexpr float MIN_SPEED = 20.0f;
		/** @brief Faster than this between two steps is a teleport, not movement. */
		constexpr float MAX_SPEED = 1500.0f;
		/** @brief Per-second rate at which a body part drags the air around it toward its own velocity. */
		constexpr float PUSH_RATE = 6.0f;
		/** @brief Speed at which an actor's shove on the fog is whole (a run). */
		constexpr float FULL_SHOVE_SPEED = 350.0f;
		/** @brief Parts higher than this above the feet are above the ground fog. */
		constexpr float MAX_HEIGHT_ABOVE_FEET = 140.0f;

		/** @brief A skeleton node that stirs the fog, and the radius it stirs (world units, at scale 1). */
		struct BodyPart
		{
			const char* node;
			float radius;
		};

		/** @brief The parts of a humanoid skeleton that move through ground fog: feet, lower legs and hips. */
		constexpr std::array<BodyPart, 5> HUMANOID_PARTS = { {
			{ "NPC L Foot [Lft ]", 12.0f },
			{ "NPC R Foot [Rft ]", 12.0f },
			{ "NPC L Calf [LClf]", 11.0f },
			{ "NPC R Calf [RClf]", 11.0f },
			{ "NPC Pelvis [Pelv]", 18.0f },
		} };

		/** @brief Smallest whole-body radius, for creatures without the parts above. */
		constexpr float MIN_BODY_RADIUS = 24.0f;

		struct Candidate
		{
			RE::Actor* actor;
			float distanceSquared;
		};
	}

	void AirSources::Clear()
	{
		history.clear();
		movers.clear();
		movedNearby = false;
	}

	const std::vector<Mover>& AirSources::Update(const RE::NiPoint3& a_playerPos, float a_fineReach, float a_outerReach, float a_deltaTime, float a_strength,
		float a_shove, uint32_t a_maxMovers)
	{
		movers.clear();
		movedNearby = false;
		updateCounter++;
		if (!(a_deltaTime > 0.0f) || a_strength <= 0.0f)
			return movers;

		const float halfGrid = a_outerReach;
		std::vector<Candidate> candidates;
		const auto consider = [&](RE::Actor* a_actor) {
			if (!a_actor || !a_actor->Is3DLoaded() || a_actor->IsDisabled())
				return;
			const auto position = a_actor->GetPosition();
			if (std::abs(position.x - a_playerPos.x) > halfGrid || std::abs(position.y - a_playerPos.y) > halfGrid)
				return;
			candidates.push_back({ a_actor, a_playerPos.GetSquaredDistance(position) });
		};
		if (auto* processLists = RE::ProcessLists::GetSingleton()) {
			for (auto& handle : processLists->highActorHandles)
				consider(handle.get().get());
		}
		consider(RE::PlayerCharacter::GetSingleton());
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distanceSquared < b.distanceSquared; });

		// One mover per moving body part (or body), with history per actor and part
		const auto emit = [&](uint64_t a_key, const RE::NiPoint3& a_position, float a_radius, bool a_fine) {
			auto& seen = history[a_key];
			const bool hasPrevious = seen.lastSeen == updateCounter - 1;
			const RE::NiPoint3 previous = seen.position;
			seen.position = a_position;
			seen.lastSeen = updateCounter;
			if (!hasPrevious || movers.size() >= a_maxMovers)
				return;
			const float vx = (a_position.x - previous.x) / a_deltaTime;
			const float vy = (a_position.y - previous.y) / a_deltaTime;
			const float speed = std::sqrt(vx * vx + vy * vy);
			// A part at rest lets the fog settle back
			if (speed < MIN_SPEED || speed > MAX_SPEED)
				return;
			const float push = std::min(PUSH_RATE * a_deltaTime, 1.0f) * a_strength;
			// From where it was to where it is: a fast part sweeps a line, not a dot
			movers.push_back({ .from = { previous.x, previous.y },
				.to = { a_position.x, a_position.y },
				.radius = a_radius,
				.shove = a_shove * std::min(speed / FULL_SHOVE_SPEED, 1.0f),
				.velocity = { vx * push, vy * push },
				.carve = 0.0f,
				.pad = {} });
			movedNearby |= a_fine;
		};

		for (const auto& candidate : candidates) {
			if (movers.size() >= a_maxMovers)
				break;
			auto* actor = candidate.actor;
			const uint64_t actorKey = static_cast<uint64_t>(actor->GetFormID()) << 8;
			const float scale = actor->GetScale();
			const float feet = actor->GetPosition().z;
			auto* root = actor->Get3D(false);

			const auto actorPosition = actor->GetPosition();
			const bool fine = std::abs(actorPosition.x - a_playerPos.x) < a_fineReach && std::abs(actorPosition.y - a_playerPos.y) < a_fineReach;
			bool foundPart = false;
			if (root && fine) {
				for (std::size_t i = 0; i < HUMANOID_PARTS.size(); i++) {
					auto* node = root->GetObjectByName(HUMANOID_PARTS[i].node);
					if (!node)
						continue;
					foundPart = true;
					const auto& position = node->world.translate;
					if (position.z - feet > MAX_HEIGHT_ABOVE_FEET * scale)
						continue;
					emit(actorKey | i, position, HUMANOID_PARTS[i].radius * scale, true);
				}
			}
			// Creatures without a humanoid skeleton, and everyone past the fine level, stir the fog with their whole body
			if (!foundPart) {
				const auto boundMin = actor->GetBoundMin();
				const auto boundMax = actor->GetBoundMax();
				const float halfWidth = 0.5f * std::max(boundMax.x - boundMin.x, boundMax.y - boundMin.y) * scale;
				emit(actorKey | 0xFF, actorPosition, std::max(halfWidth, MIN_BODY_RADIUS), fine);
			}
		}

		for (auto it = history.begin(); it != history.end();) {
			if (updateCounter - it->second.lastSeen > FORGET_AFTER)
				it = history.erase(it);
			else
				++it;
		}
		return movers;
	}
}
