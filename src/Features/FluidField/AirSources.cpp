#include "AirSources.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Slower than this (world units per second) stirs nothing. */
		constexpr float MIN_SPEED = 20.0f;
		/** @brief Faster than this between two steps is a teleport, not movement. */
		constexpr float MAX_SPEED = 1500.0f;
		/** @brief Share of a body part's own velocity it gives the fog round it, at strength 1. */
		constexpr float PUSH_SHARE = 0.35f;
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

		/** @brief A leg's bones, hip to foot: each pair a capsule in the contact map. */
		constexpr std::array<std::array<const char*, 3>, 2> LEGS = { {
			{ "NPC L Thigh [LThg]", "NPC L Calf [LClf]", "NPC L Foot [Lft ]" },
			{ "NPC R Thigh [RThg]", "NPC R Calf [RClf]", "NPC R Foot [Rft ]" },
		} };
		/** @brief A leg capsule's radius at scale 1, world units. */
		constexpr float LEG_RADIUS = 9.0f;

		/** @brief Name parts of a creature's leg bones (lower case). */
		constexpr std::array<std::string_view, 9> LEG_NAMES = { "leg", "foot", "calf", "thigh", "toe", "hoof", "knee", "ankle", "paw" };
		/** @brief A creature leg capsule's radius at scale 1, world units. */
		constexpr float CREATURE_LEG_RADIUS = 6.0f;

		bool IsLegBone(RE::NiAVObject* a_object)
		{
			if (!a_object || !a_object->AsNode() || a_object->name.empty())
				return false;
			std::string name(a_object->name.c_str());
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			return std::ranges::any_of(LEG_NAMES, [&](std::string_view a_part) { return name.find(a_part) != std::string::npos; });
		}

		/** @brief Smallest whole-body radius, for creatures without the parts above. */
		constexpr float MIN_BODY_RADIUS = 24.0f;

		struct Candidate
		{
			RE::Actor* actor;
			float distanceSquared;
		};
	}

	const AirSources::Legs& AirSources::FindLegs(RE::Actor& a_actor, RE::NiAVObject& a_root)
	{
		auto& found = legs[a_actor.GetFormID()];
		found.lastSeen = updateCounter;
		if (found.root == &a_root)
			return found;
		found.root = &a_root;
		found.bones.clear();
		RE::BSVisit::TraverseScenegraphObjects(&a_root, [&](RE::NiAVObject* a_object) {
			// A leg bone and the one above it (hip to knee to foot): the bone's length as a capsule
			if (IsLegBone(a_object) && a_object->parent && IsLegBone(a_object->parent))
				found.bones.emplace_back(a_object->parent, a_object);
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return found;
	}

	void AirSources::Clear()
	{
		history.clear();
		legs.clear();
		pushers.clear();
		contacts.clear();
	}

	void AirSources::Update(const RE::NiPoint3& a_playerPos, float a_contactReach, float a_outerReach, float a_deltaTime, float a_strength, float a_shove, uint32_t a_maxMovers)
	{
		pushers.clear();
		contacts.clear();
		updateCounter++;
		if (!(a_deltaTime > 0.0f))
			return;

		std::vector<Candidate> candidates;
		const auto consider = [&](RE::Actor* a_actor) {
			if (!a_actor || !a_actor->Is3DLoaded() || a_actor->IsDisabled())
				return;
			const auto position = a_actor->GetPosition();
			if (std::abs(position.x - a_playerPos.x) > a_outerReach || std::abs(position.y - a_playerPos.y) > a_outerReach)
				return;
			candidates.push_back({ a_actor, a_playerPos.GetSquaredDistance(position) });
		};
		if (auto* processLists = RE::ProcessLists::GetSingleton()) {
			for (auto& handle : processLists->highActorHandles)
				consider(handle.get().get());
		}
		consider(RE::PlayerCharacter::GetSingleton());
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.distanceSquared < b.distanceSquared; });

		// One pusher per moving body part (or body), with history per actor and part
		const auto push = [&](uint64_t a_key, const RE::NiPoint3& a_position, float a_radius, float a_top) {
			auto& seen = history[a_key];
			const bool hasPrevious = seen.lastSeen == updateCounter - 1;
			const RE::NiPoint3 previous = seen.position;
			seen.position = a_position;
			seen.lastSeen = updateCounter;
			if (!hasPrevious || pushers.size() >= a_maxMovers || a_strength <= 0.0f)
				return;
			const float vx = (a_position.x - previous.x) / a_deltaTime;
			const float vy = (a_position.y - previous.y) / a_deltaTime;
			const float speed = std::sqrt(vx * vx + vy * vy);
			// A part at rest lets the fog settle back
			if (speed < MIN_SPEED || speed > MAX_SPEED)
				return;
			// From where it was to where it is: a fast part sweeps a line, not a dot
			pushers.push_back({ .from = { previous.x, previous.y },
				.to = { a_position.x, a_position.y },
				.radius = a_radius,
				.shove = a_shove * std::min(speed / FULL_SHOVE_SPEED, 1.0f),
				.velocity = { vx * PUSH_SHARE * a_strength, vy * PUSH_SHARE * a_strength },
				.cut = 0.0f,
				.bottom = std::min(a_position.z, previous.z) - a_radius,
				.top = a_top,
				.pad = 0.0f });
		};
		const auto contact = [&](const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_radius, float a_bottom, float a_top) {
			if (contacts.size() < a_maxMovers)
				contacts.push_back({ .from = { a_from.x, a_from.y }, .to = { a_to.x, a_to.y }, .radius = a_radius, .shove = 0.0f, .velocity = {}, .cut = 1.0f, .bottom = a_bottom, .top = a_top, .pad = 0.0f });
		};

		for (const auto& candidate : candidates) {
			auto* actor = candidate.actor;
			const uint64_t actorKey = static_cast<uint64_t>(actor->GetFormID()) << 8;
			const float scale = actor->GetScale();
			const float feet = actor->GetPosition().z;
			auto* root = actor->Get3D(false);
			const auto actorPosition = actor->GetPosition();
			const bool close = candidate.distanceSquared < a_contactReach * a_contactReach;
			bool skeleton = false;
			if (root && close) {
				for (std::size_t i = 0; i < HUMANOID_PARTS.size(); i++) {
					auto* node = root->GetObjectByName(HUMANOID_PARTS[i].node);
					if (!node)
						continue;
					skeleton = true;
					const auto& position = node->world.translate;
					if (position.z - feet <= MAX_HEIGHT_ABOVE_FEET * scale)
						push(actorKey | i, position, HUMANOID_PARTS[i].radius * scale, position.z + HUMANOID_PARTS[i].radius * scale);
				}
				// The legs as capsules, hip to knee and knee to foot
				for (const auto& leg : LEGS) {
					const RE::NiAVObject* bones[3] = { root->GetObjectByName(leg[0]), root->GetObjectByName(leg[1]), root->GetObjectByName(leg[2]) };
					for (int k = 0; k < 2; k++) {
						if (!bones[k] || !bones[k + 1])
							continue;
						const auto& a = bones[k]->world.translate;
						const auto& b = bones[k + 1]->world.translate;
						const float r = LEG_RADIUS * scale;
						contact(a, b, r, std::min(a.z, b.z) - r, std::max(a.z, b.z) + r);
					}
				}
			}
			// Creatures without a humanoid skeleton, and everyone farther out, by their whole body
			if (!skeleton) {
				const auto boundMin = actor->GetBoundMin();
				const auto boundMax = actor->GetBoundMax();
				const float radius = std::max(0.5f * std::max(boundMax.x - boundMin.x, boundMax.y - boundMin.y) * scale, MIN_BODY_RADIUS);
				const float top = feet + std::max(boundMax.z - boundMin.z, 40.0f) * scale;
				push(actorKey | 0xFF, actorPosition, radius, top);
				const Legs* creatureLegs = close && root ? &FindLegs(*actor, *root) : nullptr;
				if (creatureLegs && !creatureLegs->bones.empty()) {
					const float r = CREATURE_LEG_RADIUS * scale;
					for (const auto& [upper, lower] : creatureLegs->bones) {
						const auto& a = upper->world.translate;
						const auto& b = lower->world.translate;
						contact(a, b, r, std::min(a.z, b.z) - r, std::max(a.z, b.z) + r);
					}
				} else if (close)
					contact(actorPosition, actorPosition, radius, feet - 10.0f, top);
			}
		}

		std::erase_if(legs, [&](const auto& a_entry) { return updateCounter - a_entry.second.lastSeen > FORGET_AFTER; });
		for (auto it = history.begin(); it != history.end();) {
			if (updateCounter - it->second.lastSeen > FORGET_AFTER)
				it = history.erase(it);
			else
				++it;
		}
	}
}
