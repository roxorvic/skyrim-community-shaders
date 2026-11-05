#include "WakeSources.h"

#include "WaterSurfaces.h"

#include <algorithm>
#include <cmath>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Slower than this (world units per second) makes no wake. */
		constexpr float MIN_WAKE_SPEED = 20.0f;
		/** @brief Faster than this between two steps is a teleport, not movement. */
		constexpr float MAX_SPEED = 1500.0f;
		/** @brief Feet may be this far above the surface and still touch the water (a wading stride). */
		constexpr float FEET_ABOVE_SURFACE = 32.0f;
		/** @brief Smallest wake radius, world units, so tiny creatures still disturb the water. */
		constexpr float MIN_WAKE_RADIUS = 12.0f;
		/** @brief A wake is this fraction of the actor's horizontal half-width. */
		constexpr float WAKE_RADIUS_SCALE = 0.6f;
		/** @brief Creatures' round body in the flow, as a share of their bounds' half-width. */
		constexpr float BODY_RADIUS_SCALE = 1.2f;
		/** @brief A body circle's emitter radius over the limb's own: the shaders' solid core is about half the radius. */
		constexpr float BODY_RADIUS_SPREAD = 1.4f;
		/** @brief Marks a body emitter (its radial speed). */
		constexpr float BODY_MARK = -1.0f;

		/** @brief A humanoid limb, from one node to the next, and its radius at scale 1 (world units). */
		struct Bone
		{
			const char* from;
			const char* to;
			float radius;
		};
		constexpr std::array<Bone, 8> LIMBS = { {
			{ "NPC L Thigh [LThg]", "NPC L Calf [LClf]", 8.0f },
			{ "NPC L Calf [LClf]", "NPC L Foot [Lft ]", 6.0f },
			{ "NPC R Thigh [RThg]", "NPC R Calf [RClf]", 8.0f },
			{ "NPC R Calf [RClf]", "NPC R Foot [Rft ]", 6.0f },
			{ "NPC L UpperArm [LUar]", "NPC L Forearm [LLar]", 4.5f },
			{ "NPC L Forearm [LLar]", "NPC L Hand [LHnd]", 3.5f },
			{ "NPC R UpperArm [RUar]", "NPC R Forearm [RLar]", 4.5f },
			{ "NPC R Forearm [RLar]", "NPC R Hand [RHnd]", 3.5f },
		} };
		/** @brief The torso: pelvis to neck, an oval of three circles across the hips or shoulders. */
		constexpr std::array<const char*, 3> SPINE = { "NPC Pelvis [Pelv]", "NPC Spine2 [Spn2]", "NPC Neck [Neck]" };
		constexpr float TORSO_RADIUS = 10.0f;
		constexpr float TORSO_SIDE = 8.0f;  ///< Its side circles' offset from the middle
		/** @brief Vertical speed (world units per second) below which crossing the surface is not a splash. */
		constexpr float MIN_SPLASH_SPEED = 150.0f;
		/** @brief Depth pushed down at the entry point per unit of downward speed. */
		constexpr float SPLASH_DEPTH_PER_SPEED = 0.1f;
		/** @brief Most depth one splash pushes down, world units. */
		constexpr float MAX_SPLASH_DEPTH = 120.0f;
		/** @brief A splash covers this many times the actor's wake radius. */
		constexpr float SPLASH_RADIUS_SCALE = 2.5f;
		/** @brief Leaving the water pushes less than entering it. */
		constexpr float EXIT_SPLASH_SCALE = 0.4f;

		struct Candidate
		{
			RE::Actor* actor;
			float distanceSquared;
		};
	}

	bool WakeSources::AppendBody(RE::Actor& a_actor, float a_surface, const RE::NiPoint3& a_velocity, const int32_t a_gridMin[2], float a_cellSize, float a_deltaTime)
	{
		auto* root = a_actor.Get3D();
		if (!root)
			return false;
		const auto node = [&](const char* a_name) -> const RE::NiPoint3* {
			auto* object = root->GetObjectByName(a_name);
			return object ? &object->world.translate : nullptr;
		};
		const float scale = a_actor.GetScale();
		const uint64_t actorKey = static_cast<uint64_t>(a_actor.GetFormID()) << 8;

		// Where a bone crosses the surface; or, lying along it (swimming), its middle
		const auto crossing = [&](const RE::NiPoint3& a, const RE::NiPoint3& b, float a_band, RE::NiPoint3& a_point) {
			if ((a.z - a_surface) * (b.z - a_surface) <= 0.0f && a.z != b.z) {
				a_point = a + (b - a) * ((a_surface - a.z) / (b.z - a.z));
				return true;
			}
			if (std::abs(a.z - a_surface) < a_band && std::abs(b.z - a_surface) < a_band) {
				a_point = (a + b) * 0.5f;
				return true;
			}
			return false;
		};
		// A solid circle, moving as that bone's crossing moved
		const auto emit = [&](uint32_t a_key, const RE::NiPoint3& a_point, float a_radius) {
			if (emitters.size() >= ShallowWaterSolver::MAX_EMITTERS - 1)
				return;
			auto& seen = boneHistory[actorKey | a_key];
			RE::NiPoint3 velocity = a_velocity;
			if (seen.lastSeen == updateCounter - 1) {
				const RE::NiPoint3 moved = (a_point - seen.position) / a_deltaTime;
				if (moved.Length() < MAX_SPEED)
					velocity = moved;
			}
			seen.position = a_point;
			seen.lastSeen = updateCounter;
			emitters.push_back({ .position = { a_point.x / a_cellSize - static_cast<float>(a_gridMin[0]), a_point.y / a_cellSize - static_cast<float>(a_gridMin[1]) },
				.radius = std::max(a_radius * scale * BODY_RADIUS_SPREAD / a_cellSize, 1.0f),
				.depthRate = 0.0f,
				.velocity = { velocity.x, velocity.y },
				.pad1 = { BODY_MARK, 0.0f } });
		};

		bool found = false;
		RE::NiPoint3 point;
		for (std::size_t i = 0; i < LIMBS.size(); i++) {
			const auto* a = node(LIMBS[i].from);
			const auto* b = node(LIMBS[i].to);
			if (!a || !b)
				continue;
			found = true;
			if (crossing(*a, *b, LIMBS[i].radius * scale, point))
				emit(static_cast<uint32_t>(i), point, LIMBS[i].radius);
		}

		// The torso, its oval across the hips low down and the shoulders higher up
		const auto* hipL = node("NPC L Thigh [LThg]");
		const auto* hipR = node("NPC R Thigh [RThg]");
		const auto* shoulderL = node("NPC L UpperArm [LUar]");
		const auto* shoulderR = node("NPC R UpperArm [RUar]");
		for (std::size_t i = 0; i + 1 < SPINE.size(); i++) {
			const auto* a = node(SPINE[i]);
			const auto* b = node(SPINE[i + 1]);
			if (!a || !b || !crossing(*a, *b, TORSO_RADIUS * scale, point))
				continue;
			found = true;
			const auto* left = i == 0 ? hipL : shoulderL;
			const auto* right = i == 0 ? hipR : shoulderR;
			RE::NiPoint3 side{};
			if (left && right) {
				side = *left - *right;
				side.z = 0.0f;
				const float length = side.Length();
				side = length > 1e-3f ? side * (TORSO_SIDE * scale / length) : RE::NiPoint3{};
			}
			const uint32_t key = static_cast<uint32_t>(LIMBS.size() + 3 * i);
			emit(key, point, TORSO_RADIUS);
			if (side.Length() > 0.0f) {
				emit(key + 1, point + side, TORSO_RADIUS * 0.8f);
				emit(key + 2, point - side, TORSO_RADIUS * 0.8f);
			}
		}
		return found;
	}

	void WakeSources::Clear()
	{
		history.clear();
		boneHistory.clear();
		emitters.clear();
	}

	const std::vector<WaterEmitter>& WakeSources::Update(const WaterSurfaces& a_water, const RE::NiPoint3& a_playerPos, const int32_t a_gridMin[2],
		float a_cellSize, uint32_t a_gridRes, float a_deltaTime, const Settings& a_settings)
	{
		emitters.clear();
		updateCounter++;
		if (!(a_deltaTime > 0.0f) || !(a_cellSize > 0.0f))
			return emitters;

		const float halfGrid = a_cellSize * static_cast<float>(a_gridRes) * 0.5f;
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

		for (const auto& candidate : candidates) {
			auto* actor = candidate.actor;
			const uint32_t formID = actor->GetFormID();
			const auto position = actor->GetPosition();

			// Velocity from the last sighting; a first sighting or a teleport gives none
			auto& seen = history[formID];
			const bool hasPrevious = seen.lastSeen == updateCounter - 1;
			const RE::NiPoint3 previous = seen.position;
			seen.position = position;
			seen.lastSeen = updateCounter;
			if (!hasPrevious || emitters.size() >= ShallowWaterSolver::MAX_EMITTERS)
				continue;
			const RE::NiPoint3 velocity = (position - previous) / a_deltaTime;
			if (velocity.Length() > MAX_SPEED)
				continue;

			float surface = 0.0f;
			if (a_water.SampleHeight(position.x, position.y, surface) == WaterSurfaces::Source::None)
				continue;

			// The actor's bound, scaled: its top decides whether it is fully under water
			const float scale = actor->GetScale();
			const auto boundMin = actor->GetBoundMin();
			const auto boundMax = actor->GetBoundMax();
			const float top = position.z + boundMax.z * scale;
			const float halfWidth = 0.5f * std::max(boundMax.x - boundMin.x, boundMax.y - boundMin.y) * scale;
			const float radius = std::max(halfWidth * WAKE_RADIUS_SCALE, MIN_WAKE_RADIUS) / a_cellSize;
			const float texel[2] = { position.x / a_cellSize - static_cast<float>(a_gridMin[0]), position.y / a_cellSize - static_cast<float>(a_gridMin[1]) };

			// Entering or leaving the water: the feet crossed the surface this step
			const bool wasAbove = previous.z > surface;
			const bool isAbove = position.z > surface;
			if (a_settings.splashStrength > 0.0f && wasAbove != isAbove && std::abs(velocity.z) > MIN_SPLASH_SPEED) {
				const float push = std::min(std::abs(velocity.z) * SPLASH_DEPTH_PER_SPEED, MAX_SPLASH_DEPTH) * a_settings.splashStrength * (isAbove ? EXIT_SPLASH_SCALE : 1.0f);
				emitters.push_back({ .position = { texel[0], texel[1] },
					.radius = radius * SPLASH_RADIUS_SCALE,
					.depthRate = -push / a_deltaTime,  // Spread over this frame's sub-steps
					.velocity = { velocity.x, velocity.y },
					.pad1 = {} });
				continue;
			}

			// A body: in the water and not under it, moving or not, the flow parts around it
			if (position.z <= surface + FEET_ABOVE_SURFACE && top >= surface && !AppendBody(*actor, surface, velocity, a_gridMin, a_cellSize, a_deltaTime) &&
				emitters.size() < ShallowWaterSolver::MAX_EMITTERS - 1)
				emitters.push_back({ .position = { texel[0], texel[1] },
					.radius = std::max(halfWidth * BODY_RADIUS_SCALE, MIN_WAKE_RADIUS) / a_cellSize,
					.depthRate = 0.0f,
					.velocity = { velocity.x, velocity.y },
					.pad1 = { BODY_MARK, 0.0f } });

			// A wake: in the water, not fully under it, and moving
			const float speed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
			if (a_settings.wakeStrength <= 0.0f || speed < MIN_WAKE_SPEED || position.z > surface + FEET_ABOVE_SURFACE || top < surface)
				continue;
			emitters.push_back({ .position = { texel[0], texel[1] },
				.radius = radius,
				.depthRate = 0.0f,
				.velocity = { velocity.x * a_settings.wakeStrength, velocity.y * a_settings.wakeStrength },
				.pad1 = {} });
		}

		for (auto it = history.begin(); it != history.end();) {
			if (updateCounter - it->second.lastSeen > FORGET_AFTER)
				it = history.erase(it);
			else
				++it;
		}
		for (auto it = boneHistory.begin(); it != boneHistory.end();) {
			if (updateCounter - it->second.lastSeen > FORGET_AFTER)
				it = boneHistory.erase(it);
			else
				++it;
		}
		return emitters;
	}
}
