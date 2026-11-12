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

		/** @brief A weapon's blade capsule radius; the push its tip's swing gives, as a share of the tip's speed. */
		constexpr float BLADE_RADIUS = 5.0f;
		constexpr float SWING_PUSH = 0.5f;
		/** @brief Projectiles: push as a share of their speed (capped), and their capsules' radii (arrows, others). */
		constexpr float PROJECTILE_PUSH = 0.3f;
		constexpr float MAX_PROJECTILE_PUSH = 400.0f;
		constexpr float ARROW_RADIUS = 6.0f;
		constexpr float SPELL_RADIUS = 18.0f;
		/** @brief A projectile slower than this has landed (an arrow stuck in the ground): it no longer moves the fog. */
		constexpr float LANDED_SPEED = 50.0f;
		/** @brief Faster than this between two updates is a jump (spawned, reused), not flight. */
		constexpr float MAX_PROJECTILE_SPEED = 20000.0f;
		/** @brief How far below its path a projectile's wake parts the fog; how much a spell's heat or chill thins it there. */
		constexpr float PROJECTILE_WAKE = 100.0f;
		constexpr float SPELL_CUT = 0.3f;
		/** @brief A spell's wake: its width, and how hard it shoves the fog aside. */
		constexpr float WAKE_RADIUS = 50.0f;
		constexpr float SPELL_SHOVE = 200.0f;
		/** @brief Explosions shove for this long after they start, this hard, and cut this much. */
		constexpr float BLAST_SECONDS = 0.4f;
		constexpr float BLAST_SHOVE = 900.0f;
		constexpr float BLAST_CUT = 0.6f;
		/** @brief A shout pushes this long, this far ahead, this fast. */
		constexpr float SHOUT_SECONDS = 0.8f;
		constexpr float SHOUT_REACH = 900.0f;
		constexpr float SHOUT_PUSH = 700.0f;
		/** @brief Fires: how fast the fog shoved from them flows, and the share of it cleared each step. */
		constexpr float HEAT_SHOVE = 150.0f;
		constexpr float HEAT_CUT = 0.12f;
		/** @brief A dead body's torso capsule radius at scale 1. */
		constexpr float TORSO_RADIUS = 16.0f;

		/** @brief Explosions seen by the hook since the last update: where, how wide, how old. */
		struct Blast
		{
			RE::NiPoint3 position;
			float radius, age;
		};
		std::mutex blastMutex;
		std::vector<Blast> blasts;
		std::vector<RE::ObjectRefHandle> shouters;  ///< Shouts cast since the last update (under blastMutex)

		struct ExplosionUpdate
		{
			static void thunk(RE::Explosion* a_this, float a_delta)
			{
				func(a_this, a_delta);
				const auto& data = a_this->GetExplosionRuntimeData();
				if (data.age < BLAST_SECONDS) {
					std::scoped_lock lock(blastMutex);
					if (blasts.size() < 64)
						blasts.push_back({ a_this->GetPosition(), data.radius, data.age });
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ShoutListener : RE::BSTEventSink<RE::TESSpellCastEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* a_event, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				const auto* spell = a_event ? RE::TESForm::LookupByID<RE::MagicItem>(a_event->spell) : nullptr;
				if (spell && a_event->object && spell->GetSpellType() == RE::MagicSystem::SpellType::kVoicePower) {
					std::scoped_lock lock(blastMutex);
					shouters.push_back(a_event->object->GetHandle());
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

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
		found.bones.clear();
		RE::BSVisit::TraverseScenegraphObjects(&a_root, [&](RE::NiAVObject* a_object) {
			// A leg bone and the one above it (hip to knee to foot): the bone's length as a capsule
			if (IsLegBone(a_object) && a_object->parent && IsLegBone(a_object->parent))
				found.bones.emplace_back(a_object->parent, a_object);
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return found;
	}

	void AirSources::Install()
	{
		static bool installed = false;
		if (installed)
			return;
		installed = true;
		stl::write_vfunc<0xA3, ExplosionUpdate>(RE::VTABLE_Explosion[0]);
		static ShoutListener listener;
		if (auto* events = RE::ScriptEventSourceHolder::GetSingleton())
			events->AddEventSink<RE::TESSpellCastEvent>(&listener);
	}

	void AirSources::Clear()
	{
		shouts.clear();
		projectiles.clear();
		history.clear();
		legs.clear();
		pushers.clear();
		contacts.clear();
	}

	void AirSources::Update(const RE::NiPoint3& a_playerPos, float a_contactReach, float a_outerReach, float a_deltaTime, float a_strength, float a_shove, uint32_t a_maxMovers,
		const std::vector<HeatSource>& a_heat)
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
			// A drawn weapon close by: the blade cuts, its swing pushes (D194 step 6)
			const auto* state = actor->AsActorState();
			if (root && close && state && state->IsWeaponDrawn()) {
				for (const char* attach : { "WEAPON", "SHIELD" }) {
					auto* node = root->GetObjectByName(attach);
					if (!node || node->worldBound.radius < 8.0f)
						continue;
					const auto& grip = node->world.translate;
					const RE::NiPoint3 tip = node->worldBound.center * 2.0f - grip;
					const uint64_t key = actorKey | (attach[0] == 'W' ? 0x40 : 0x41);
					auto& seen = history[key];
					const bool hasPrevious = seen.lastSeen == updateCounter - 1;
					const RE::NiPoint3 previousTip = hasPrevious ? seen.position : tip;
					seen.position = tip;
					seen.lastSeen = updateCounter;
					contact(grip, tip, BLADE_RADIUS * scale, std::min(grip.z, tip.z) - BLADE_RADIUS, std::max(grip.z, tip.z) + BLADE_RADIUS);
					// The tip's sweep since the last step: a slash cuts a sheet, not a line
					contact(previousTip, tip, BLADE_RADIUS * scale, std::min(previousTip.z, tip.z) - BLADE_RADIUS, std::max(previousTip.z, tip.z) + BLADE_RADIUS);
					const RE::NiPoint3 swing = (tip - previousTip) / a_deltaTime;
					if (hasPrevious && swing.Length() > MIN_SPEED && pushers.size() < a_maxMovers)
						pushers.push_back({ .from = { previousTip.x, previousTip.y }, .to = { tip.x, tip.y }, .radius = 20.0f * scale, .shove = 0.0f, .velocity = { swing.x * SWING_PUSH * a_strength, swing.y * SWING_PUSH * a_strength }, .cut = 0.0f, .bottom = std::min(previousTip.z, tip.z) - BLADE_RADIUS, .top = std::max(previousTip.z, tip.z) + BLADE_RADIUS, .pad = 0.0f });
				}
			}
			// A dead body (a ragdoll) lies in the fog: its torso cuts too
			if (root && close && actor->IsDead()) {
				auto* pelvis = root->GetObjectByName("NPC Pelvis [Pelv]");
				auto* neck = root->GetObjectByName("NPC Neck [Neck]");
				if (pelvis && neck) {
					const auto& a = pelvis->world.translate;
					const auto& b = neck->world.translate;
					const float r = TORSO_RADIUS * scale;
					contact(a, b, r, std::min(a.z, b.z) - r, std::max(a.z, b.z) + r);
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

		// Projectiles in flight: they push along their path and cut it
		if (auto* manager = RE::Projectile::Manager::GetSingleton()) {
			RE::BSSpinLockGuard lock(manager->projectileLock);
			for (const auto* list : { &manager->limited, &manager->unlimited }) {
				for (const auto& handle : *list) {
					const auto projectile = handle.get();
					if (!projectile || pushers.size() >= a_maxMovers)
						continue;
					const auto position = projectile->GetPosition();
					if (std::abs(position.x - a_playerPos.x) > a_outerReach || std::abs(position.y - a_playerPos.y) > a_outerReach)
						continue;
					// Beams and flames stand at the caster while they reach out: by their 3D's extent, moving or not
					if (projectile->As<RE::BeamProjectile>() || projectile->As<RE::ConeProjectile>() || projectile->As<RE::FlameProjectile>()) {
						const auto* root = projectile->Get3D();
						if (!root || root->worldBound.radius < 1.0f)
							continue;
						const RE::NiPoint3 reach = root->worldBound.center * 2.0f - position;
						const float r = std::max(root->worldBound.radius * 0.25f, SPELL_RADIUS);
						pushers.push_back({ .from = { position.x, position.y }, .to = { reach.x, reach.y }, .radius = r, .shove = SPELL_SHOVE * a_strength, .velocity = {}, .cut = SPELL_CUT, .bottom = std::min(position.z, reach.z) - r - PROJECTILE_WAKE, .top = std::max(position.z, reach.z) + r, .pad = 0.0f });
						continue;
					}
					// Its motion since the last update: a landed one (an arrow stuck in the ground) no longer touches the fog
					auto& [last, seen] = projectiles[projectile->GetFormID()];
					const bool tracked = seen == updateCounter - 1;
					const RE::NiPoint3 from = last;
					last = position;
					seen = updateCounter;
					const RE::NiPoint3 velocity = (position - from) / a_deltaTime;
					const float speed = velocity.Length();
					if (!tracked || speed < LANDED_SPEED || speed > MAX_PROJECTILE_SPEED)
						continue;
					const bool arrow = projectile->As<RE::ArrowProjectile>() != nullptr;
					const float r = arrow ? ARROW_RADIUS : SPELL_RADIUS;
					const float share = speed > 0.0f ? std::min(speed * PROJECTILE_PUSH, MAX_PROJECTILE_PUSH) / speed * a_strength : 0.0f;
					// Its wake parts the fog under its path too, and a spell's heat or chill thins it
					pushers.push_back({ .from = { from.x, from.y }, .to = { position.x, position.y }, .radius = arrow ? r * 2.0f : WAKE_RADIUS, .shove = arrow ? 0.0f : SPELL_SHOVE * a_strength, .velocity = { velocity.x * share, velocity.y * share }, .cut = arrow ? 0.0f : SPELL_CUT, .bottom = std::min(from.z, position.z) - r - PROJECTILE_WAKE, .top = std::max(from.z, position.z) + r, .pad = 0.0f });
					if (a_playerPos.GetSquaredDistance(position) < a_contactReach * a_contactReach)
						contact(from, position, r, std::min(from.z, position.z) - r, std::max(from.z, position.z) + r);
				}
			}
		}
		// Explosions and shouts the hooks saw
		std::vector<Blast> newBlasts;
		{
			std::scoped_lock lock(blastMutex);
			newBlasts.swap(blasts);
			for (const auto& shouter : shouters)
				shouts.emplace_back(shouter, SHOUT_SECONDS);
			shouters.clear();
		}
		for (const auto& blast : newBlasts) {
			if (pushers.size() >= a_maxMovers)
				break;
			const float r = std::max(blast.radius * 0.5f, 60.0f);
			const float fade = 1.0f - blast.age / BLAST_SECONDS;
			pushers.push_back({ .from = { blast.position.x, blast.position.y }, .to = { blast.position.x, blast.position.y }, .radius = r, .shove = BLAST_SHOVE * fade * a_strength, .velocity = {}, .cut = BLAST_CUT * fade, .bottom = blast.position.z - blast.radius, .top = blast.position.z + blast.radius, .pad = 0.0f });
		}
		for (auto& [shouter, left] : shouts) {
			const auto ref = shouter.get();
			left -= a_deltaTime;
			if (!ref || pushers.size() + 4 > a_maxMovers)
				continue;
			// A cone ahead: four pushers, wider and farther out
			const float heading = ref->GetAngleZ();
			const float dx = std::sin(heading), dy = std::cos(heading);
			const auto origin = ref->GetPosition();
			for (int k = 1; k <= 4; k++) {
				const float along = SHOUT_REACH * k / 4.0f, r = 40.0f + along * 0.3f;
				pushers.push_back({ .from = { origin.x + dx * along * 0.75f, origin.y + dy * along * 0.75f }, .to = { origin.x + dx * along, origin.y + dy * along }, .radius = r, .shove = 200.0f * a_strength, .velocity = { dx * SHOUT_PUSH * a_strength, dy * SHOUT_PUSH * a_strength }, .cut = 0.1f, .bottom = origin.z - 50.0f, .top = origin.z + 200.0f, .pad = 0.0f });
			}
		}
		std::erase_if(shouts, [](const auto& a_shout) { return a_shout.second <= 0.0f; });

		// Fires: their heat dries the air, so the fog clears round them; a gentle outward push churns the edge, which lifts it
		for (const auto& fire : a_heat) {
			if (pushers.size() >= a_maxMovers)
				break;
			if (std::abs(fire.x - a_playerPos.x) > a_outerReach || std::abs(fire.y - a_playerPos.y) > a_outerReach)
				continue;
			pushers.push_back({ .from = { fire.x, fire.y }, .to = { fire.x, fire.y }, .radius = fire.radius, .shove = HEAT_SHOVE, .velocity = {}, .cut = HEAT_CUT, .bottom = fire.z - 100.0f, .top = fire.z + 200.0f, .pad = 0.0f });
		}

		std::erase_if(legs, [&](const auto& a_entry) { return updateCounter - a_entry.second.lastSeen > FORGET_AFTER; });
		std::erase_if(projectiles, [&](const auto& a_entry) { return updateCounter - a_entry.second.second > 1; });
		for (auto it = history.begin(); it != history.end();) {
			if (updateCounter - it->second.lastSeen > FORGET_AFTER)
				it = history.erase(it);
			else
				++it;
		}
	}
}
