#include "GroundTiles.h"

#include "Utils/FileSystem.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <string>
#include <string_view>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Model folders left out: they neither carry the fog nor stop it. */
		constexpr std::array<std::string_view, 7> IGNORED = { "plants\\", "\\plants\\", "clutter\\", "effects\\", "marker", "critters\\", "fxwaterfall" };
		/** @brief Model folders whose every triangle is rock: things standing on the ground, not ground. Mountains too: their
		 * slabs' flat tops are rock, not ground the fog lies on (MountainTrimSlab, 000A18FE, Riverwood). Registry: fog-v2.md. */
		constexpr std::array<std::string_view, 6> WALLS = { "\\rocks\\", "\\trees\\", "\\logs\\", "\\stumps\\", "treelog", "\\mountains\\" };
		/** @brief Things whose floors face up near the ground but hold no fog (it formed inside a boat, Riverwood): solid like rock.
		 * Tents stay ground (fog in an open tent is fine). Registry: fog-v3.md. */
		constexpr std::array<std::string_view, 7> HOLLOWS = { "boat", "ship", "cart", "wagon", "furniture\\", "coffin", "trough" };
		/** @brief Model names of fires: the fog clears round them. */
		constexpr std::array<std::string_view, 9> FIRES = { "campfire", "firepit", "fireplace", "brazier", "forge", "smelter", "hearth", "fxfire", "fire0" };
		/** @brief A fire clears this far past its model's edge (the falloff fades on beyond), at least LEAST_HEAT, at most MOST_HEAT. */
		constexpr float HEAT_MARGIN = 24.0f;
		constexpr float LEAST_HEAT = 40.0f;
		constexpr float MOST_HEAT = 400.0f;

		/** @brief A triangle facing up at least this much is ground; less, a wall. Landscape meshes' faces up to TERRAIN_GROUND_FACING
		 * are ground too, flagged steep (taken for walls, the landscape under them showed through). */
		constexpr float GROUND_FACING = 0.5f;
		constexpr float TERRAIN_GROUND_FACING = 0.1f;
		/** @brief A tree's triangles count as rock only this close to the ground: its trunk, not its canopy's footprint. */
		constexpr float TRUNK_REACH = 200.0f;
		/** @brief A tree's triangle facing up or down more than this is a branch, not its trunk. */
		constexpr float TRUNK_FACING = 0.5f;
		/** @brief Rock standing less than this over the ground is ground (a log's flat base plate, debris lying on the grass). */
		constexpr float ROCK_RISE = 16.0f;
		/** @brief A triangle facing down this much is an underside (an overhang's); facing down from LEAN_FACING, only under a
		 * top less than LIP_THICKNESS over it: a curved lip's steeper faces count, a leaning cliff's face does not (fog drew up
		 * it, SS 2508; without them a slab's lip had holes, SS 2509). */
		constexpr float UNDERSIDE_FACING = -0.3f;
		constexpr float LEAN_FACING = -0.1f;
		constexpr float LIP_THICKNESS = 120.0f;
		/** @brief Texels out to which an underside fills the holes in a curved one (faces too steep to count, texel centres
		 * between its triangles), where the top stands well over the land. */
		constexpr int UNDERSIDE_FILL = 3;
		/** @brief An overhang's underside clears the floor under it by at least this much. */
		constexpr float OVERHANG_CLEARANCE = 40.0f;
		/** @brief A gap under an overhang lower than this is solid (roots, a lip resting on the ground): no level there. */
		constexpr float LEAST_GAP = 24.0f;
		/** @brief The lower level's flags carry the gap over its floor (to the underside) in their higher bits: flags + gap * GAP_SCALE. */
		constexpr float GAP_SCALE = 32.0f;
		/** @brief A slope rising this much per texel on both sides of one is steep (~50 deg); a single step is a ledge, not a slope. */
		constexpr float STEEP_RISE = 1.2f * GroundTiles::TEXEL;

		/** @brief Bump when the build changes: older cached tiles are ignored. */
		constexpr uint32_t CACHE_VERSION = 15;
		/** @brief Indoors, a mesh floor within this height of the navmesh's is taken for its exact height (navmeshes float a little
		 * over the floor). */
		constexpr float NAVMESH_REACH = 48.0f;
		/** @brief Indoors, the navmesh's floor is widened this many texels into the gaps it leaves at walls and props (48 units),
		 * so fog reaches the walls. */
		constexpr int32_t NAVMESH_WIDEN = 6;
		/** @brief Indoors, a rock top this close to a neighbouring ground top is a step's or ledge's edge (its riser's faces), not a
		 * rock: ground (item 9; as rock the draw skipped it and took the floor below, and fog hung over the riser). */
		constexpr float STEP_EDGE = 8.0f;
		/** @brief Indoors, seconds after entering before a tile may settle (outdoors the landscape's loading gives the
		 * statics' 3D that time: indoors, tiles settled empty before the room had loaded, and were cached so). */
		constexpr double INTERIOR_SETTLE_SECONDS = 5.0;
		constexpr uint32_t CACHE_MAGIC = 0x31544746;  // "FGT1"

		/** @brief Read budgets on the main thread, ms: in play, and just after a load (the screen is still fading in). */
		constexpr double READ_BUDGET_MS = 0.08;
		constexpr double LOAD_READ_BUDGET_MS = 1.0;
		constexpr double LOAD_BOOST_SECONDS = 3.0;
		/** @brief Tiles built (inputs copied) and uploaded per frame. */
		constexpr int BUILDS_PER_FRAME = 2;
		constexpr int UPLOADS_PER_FRAME = 4;
		/** @brief A provisional tile is rebuilt at most this often, and looked at (settled yet?) this often. */
		constexpr double REBUILD_SECONDS = 1.0;
		constexpr double CHECK_SECONDS = 0.5;
		/** @brief Reach of the reference scans: past the window's corners. */
		constexpr float SCAN_RADIUS = (GroundTiles::SLOTS / 2 + 1) * GroundTiles::TILE_UNITS * 1.5f;
		/** @brief A reference waiting this long for its 3D is dropped (disabled, never loads). */
		constexpr double PENDING_SECONDS = 10.0;

		enum class ModelClass
		{
			Ignored,
			Walls,
			Tree,
			ByFacing,
			Terrain  ///< By facing, ground at any height over the landscape (a ledge's top over the floor under it)
		};

		ModelClass Classify(const RE::TESObjectREFR& a_ref)
		{
			const auto* base = a_ref.GetBaseObject();
			if (!base || !(base->Is(RE::FormType::Static) || base->Is(RE::FormType::MovableStatic) || base->Is(RE::FormType::Tree)))
				return ModelClass::Ignored;
			const auto* model = base->As<RE::TESModel>();
			std::string path = model && model->GetModel() ? model->GetModel() : "";
			std::transform(path.begin(), path.end(), path.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			const auto has = [&](std::string_view a_part) { return path.find(a_part) != std::string::npos; };
			if (path.empty() || std::ranges::any_of(IGNORED, has))
				return ModelClass::Ignored;
			if (base->Is(RE::FormType::Tree) || has("\\trees\\"))
				return ModelClass::Tree;
			if (std::ranges::any_of(WALLS, has) || std::ranges::any_of(HOLLOWS, has))
				return ModelClass::Walls;
			return path.starts_with("landscape\\") ? ModelClass::Terrain : ModelClass::ByFacing;
		}

		/** @brief How far round a reference the fog clears if it is a fire (any form with a fire's model), else 0. */
		float HeatReach(RE::TESObjectREFR& a_ref)
		{
			const auto* base = a_ref.GetBaseObject();
			const auto* model = base ? base->As<RE::TESModel>() : nullptr;
			if (!model || !model->GetModel())
				return 0.0f;
			std::string path = model->GetModel();
			std::transform(path.begin(), path.end(), path.begin(), [](unsigned char a_c) { return static_cast<char>(std::tolower(a_c)); });
			if (std::ranges::none_of(FIRES, [&](std::string_view a_part) { return path.find(a_part) != std::string::npos; }))
				return 0.0f;
			// The model's own box (its 3D's bounds hold the flames and smoke too), a little past its edge
			const auto size = a_ref.GetBoundMax() - a_ref.GetBoundMin();
			return std::clamp(0.5f * std::max(size.x, size.y) * a_ref.GetScale() + HEAT_MARGIN, LEAST_HEAT, MOST_HEAT);
		}

		/** @brief The cross product's z of two edges: twice the triangle's signed area seen from above. */
		float Area2(const float* a_a, const float* a_b, const float* a_c)
		{
			return (a_b[0] - a_a[0]) * (a_c[1] - a_a[1]) - (a_b[1] - a_a[1]) * (a_c[0] - a_a[0]);
		}

		/** @brief The worldspace whose land the current one uses. */
		RE::TESWorldSpace* GetLandWorldspace()
		{
			auto* tes = RE::TES::GetSingleton();
			auto* worldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
			while (worldspace && worldspace->parentWorld && worldspace->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLandData))
				worldspace = worldspace->parentWorld;
			return worldspace;
		}

		/** @brief FNV-1a of the load order and the cache version: a new mod list starts a new cache. */
		uint64_t LoadOrderHash()
		{
			uint64_t hash = 14695981039346656037ull;
			const auto add = [&](const void* a_data, std::size_t a_size) {
				for (std::size_t i = 0; i < a_size; i++)
					hash = (hash ^ static_cast<const uint8_t*>(a_data)[i]) * 1099511628211ull;
			};
			add(&CACHE_VERSION, sizeof(CACHE_VERSION));
			if (auto* data = RE::TESDataHandler::GetSingleton()) {
				if (const auto mods = data->GetLoadedMods())
					for (uint32_t i = 0; i < data->GetLoadedModCount(); i++)
						if (mods[i] && mods[i]->fileName)
							add(mods[i]->fileName, std::strlen(mods[i]->fileName) + 1);
				if (const auto mods = data->GetLoadedLightMods())
					for (uint32_t i = 0; i < data->GetLoadedLightModCount(); i++)
						if (mods[i] && mods[i]->fileName)
							add(mods[i]->fileName, std::strlen(mods[i]->fileName) + 1);
			}
			return hash;
		}
	}

	void GroundMeshes::Clear()
	{
		buckets.clear();
		seen.clear();
		queue.clear();
		dropped += changes.size();
		changes.clear();
		framesToScan = 0;
		waiting.clear();
		scanCount = 0;
		scanRadius = 0.0f;
	}

	void GroundMeshes::ChangesSince(uint64_t a_version, std::vector<Rect>& o_rects) const
	{
		const uint64_t from = a_version > dropped ? a_version - dropped : 0;
		for (uint64_t i = from; i < changes.size(); i++)
			o_rects.push_back(changes[i]);
	}

	void GroundMeshes::Copy(const float a_min[2], const float a_max[2], std::vector<Triangle>& o_triangles) const
	{
		const auto x0 = static_cast<int32_t>(std::floor(a_min[0] / BUCKET)), x1 = static_cast<int32_t>(std::floor(a_max[0] / BUCKET));
		const auto y0 = static_cast<int32_t>(std::floor(a_min[1] / BUCKET)), y1 = static_cast<int32_t>(std::floor(a_max[1] / BUCKET));
		for (int32_t y = y0; y <= y1; y++)
			for (int32_t x = x0; x <= x1; x++)
				if (const auto it = buckets.find(Key(x, y)); it != buckets.end())
					o_triangles.insert(o_triangles.end(), it->second.begin(), it->second.end());
	}

	bool GroundMeshes::Settled(const Rect& a_rect, uint32_t a_afterScan) const
	{
		if (scanCount <= a_afterScan)
			return false;
		// Inside the last scan's reach, and nothing found near it still waiting to be read
		for (const float x : { a_rect.min[0], a_rect.max[0] })
			for (const float y : { a_rect.min[1], a_rect.max[1] })
				if (std::hypot(x - scanCentre[0], y - scanCentre[1]) > scanRadius)
					return false;
		return std::ranges::none_of(queue, [&](const Pending& a_pending) {
			return a_pending.x > a_rect.min[0] - PENDING_REACH && a_pending.x < a_rect.max[0] + PENDING_REACH && a_pending.y > a_rect.min[1] - PENDING_REACH && a_pending.y < a_rect.max[1] + PENDING_REACH;
		});
	}

	void GroundMeshes::Update(const LandscapeCache& a_landscape, float a_centreX, float a_centreY, float a_radius, double a_budgetMs, const std::function<bool(const Rect&)>& a_skip)
	{
		auto* tes = RE::TES::GetSingleton();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!tes || !player)
			return;
		// Look for new references every so often (and at once after a Clear)
		if (framesToScan == 0 || framesToScan-- == 0) {
			framesToScan = SCAN_INTERVAL;
			heat.clear();
			tes->ForEachReferenceInRange(player, a_radius, [&](RE::TESObjectREFR* a_ref) {
				if (a_ref && !a_ref->IsDisabled())
					if (const float reach = HeatReach(*a_ref); reach > 0.0f) {
						// Round its model's middle, not its origin (a forge's is at one end, its glowing pit at the other)
						const auto* model = a_ref->Get3D();
						const RE::NiPoint3 centre = model ? model->worldBound.center : a_ref->GetPosition();
						heat.push_back({ centre.x, centre.y, a_ref->GetPositionZ(), reach });
					}
				if (a_ref && Classify(*a_ref) != ModelClass::Ignored && seen.try_emplace(a_ref->GetFormID(), a_ref->GetPositionX(), a_ref->GetPositionY()).second)
					queue.push_back({ a_ref->GetHandle(), a_ref->GetPositionX(), a_ref->GetPositionY() });
				return RE::BSContainer::ForEachResult::kContinue;
			});
			if (heat.size() != loggedHeat) {
				loggedHeat = heat.size();
				logger::info("[Fluid Field] Ground fog: {} fires round the player{}", heat.size(),
					heat.empty() ? std::string() : std::format(", the first at ({:.0f}, {:.0f}, {:.0f}) clearing {:.0f}", heat[0].x, heat[0].y, heat[0].z, heat[0].radius));
			}
			scanCount++;
			scanCentre[0] = a_centreX;
			scanCentre[1] = a_centreY;
			scanRadius = a_radius;
			// Buckets far behind the player are dropped, and the references read there, so they are read again on the way back
			const float keep = a_radius + 2.0f * BUCKET;
			std::erase_if(buckets, [&](const auto& a_bucket) {
				const float x = (static_cast<float>(static_cast<int32_t>(a_bucket.first >> 32)) + 0.5f) * BUCKET;
				const float y = (static_cast<float>(static_cast<int32_t>(a_bucket.first & 0xFFFFFFFF)) + 0.5f) * BUCKET;
				return std::abs(x - a_centreX) > keep || std::abs(y - a_centreY) > keep;
			});
			std::erase_if(seen, [&](const auto& a_seen) { return std::abs(a_seen.second.first - a_centreX) > a_radius + BUCKET || std::abs(a_seen.second.second - a_centreY) > a_radius + BUCKET; });
		}
		const auto start = std::chrono::steady_clock::now();
		const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(a_budgetMs));
		for (std::size_t tries = queue.size(); tries > 0 && std::chrono::steady_clock::now() < deadline; tries--) {
			const Pending pending = queue.front();
			queue.pop_front();
			const auto ref = pending.handle.get();
			if (!ref || ref->IsDisabled() || ref->IsDeleted())
				continue;
			if (!ref->Is3DLoaded()) {
				// Its 3D may still be streaming in: wait for it a while
				const auto [it, added] = waiting.try_emplace(ref->GetFormID(), start);
				if (start - it->second < std::chrono::duration<double>(PENDING_SECONDS))
					queue.push_back(pending);
				else
					waiting.erase(it);
				continue;
			}
			waiting.erase(ref->GetFormID());
			const auto* root = ref->Get3D();
			if (!root)
				continue;
			const auto& bound = root->worldBound;
			const Rect rect{ { bound.center.x - bound.radius, bound.center.y - bound.radius }, { bound.center.x + bound.radius, bound.center.y + bound.radius } };
			if (!a_skip(rect))
				Read(*ref.get(), a_landscape);
		}
		// The log of changed rectangles keeps its last few thousand (a consumer further behind rebuilds what it has)
		if (changes.size() > 8192) {
			changes.erase(changes.begin(), changes.begin() + 4096);
			dropped += 4096;
		}
	}

	void GroundMeshes::Read(RE::TESObjectREFR& a_ref, const LandscapeCache& a_landscape)
	{
		const ModelClass modelClass = Classify(a_ref);
		auto* root = a_ref.Get3D();
		if (modelClass == ModelClass::Ignored || !root)
			return;
		Rect bounds{ { FLT_MAX, FLT_MAX }, { -FLT_MAX, -FLT_MAX } };
		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
			auto* triShape = a_geometry->AsTriShape();
			auto* renderer = a_geometry->GetGeometryRuntimeData().rendererData;
			if (!triShape || !renderer || !renderer->rawVertexData || !renderer->rawIndexData)
				return RE::BSVisit::BSVisitControl::kContinue;
			// Only what is drawn lit and solid: hidden shapes and effect planes are not ground (a dirt cliff's hidden lid hung over the
			// slope below it, DirtCliffsCornerIn01, 0001924B, Riverwood)
			if (!netimmerse_cast<RE::BSLightingShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get()))
				return RE::BSVisit::BSVisitControl::kContinue;
			// A rock's or log's cut-out and see-through cards (twigs, branches, leaves) are not solid; roads blend their edges, so
			// ground keeps them
			if (const auto* alpha = (modelClass == ModelClass::Walls || modelClass == ModelClass::Tree) ? static_cast<const RE::NiAlphaProperty*>(a_geometry->GetGeometryRuntimeData().alphaProperty.get()) : nullptr;
				alpha && (alpha->GetAlphaTesting() || alpha->GetAlphaBlending()))
				return RE::BSVisit::BSVisitControl::kContinue;
			// Hidden within its own model only: above it, indoors, the game hides whole rooms out of view (portals), which
			// skipped every room the player wasn't looking into
			for (const RE::NiAVObject* node = a_geometry; node; node = node == root ? nullptr : node->parent)
				if (node->GetAppCulled())
					return RE::BSVisit::BSVisitControl::kContinue;
			const auto& counts = triShape->GetTrishapeRuntimeData();
			const uint32_t stride = renderer->vertexDesc.GetSize();
			const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			if (stride < positionOffset + 12)
				return RE::BSVisit::BSVisitControl::kContinue;
			const auto& transform = a_geometry->world;
			std::vector<RE::NiPoint3> points(counts.vertexCount);
			for (uint32_t v = 0; v < counts.vertexCount; v++) {
				float local[3];
				std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(v) * stride + positionOffset, sizeof(local));
				points[v] = transform * RE::NiPoint3(local[0], local[1], local[2]);
			}
			for (uint32_t i = 0; i < counts.triangleCount; i++) {
				const uint16_t* index = renderer->rawIndexData + static_cast<std::size_t>(i) * 3;
				if (index[0] >= counts.vertexCount || index[1] >= counts.vertexCount || index[2] >= counts.vertexCount)
					continue;
				Triangle triangle;
				const RE::NiPoint3* corners[3] = { &points[index[0]], &points[index[1]], &points[index[2]] };
				float* out[3] = { triangle.a, triangle.b, triangle.c };
				for (int k = 0; k < 3; k++) {
					out[k][0] = corners[k]->x;
					out[k][1] = corners[k]->y;
					out[k][2] = corners[k]->z;
				}
				const RE::NiPoint3 normal = (*corners[1] - *corners[0]).Cross(*corners[2] - *corners[0]);
				const float length = normal.Length();
				if (length < 1e-6f)
					continue;
				const float facing = normal.z / length;
				const float cx = (triangle.a[0] + triangle.b[0] + triangle.c[0]) / 3.0f, cy = (triangle.a[1] + triangle.b[1] + triangle.c[1]) / 3.0f;
				const float cz = std::min({ triangle.a[2], triangle.b[2], triangle.c[2] });
				float landHeight = 0.0f;
				const bool overLand = a_landscape.SampleHeight(cx, cy, landHeight);
				if (modelClass == ModelClass::Tree) {
					// The trunk's sides only: branch and needle cards over the ground are not rock
					if ((overLand && cz > landHeight + TRUNK_REACH) || std::abs(facing) > TRUNK_FACING)
						continue;
					triangle.kind = Kind::Wall;
				} else if (modelClass == ModelClass::Walls) {
					// A rock overhang's underside too: the floor under it is a second level (MountainTrimSlab ledges, Riverwood)
					triangle.kind = facing <= LEAN_FACING ? Kind::Underside : Kind::Wall;
					triangle.lean = facing > UNDERSIDE_FACING;
					triangle.flat = facing >= GROUND_FACING;
				} else if (facing >= (modelClass == ModelClass::Terrain ? TERRAIN_GROUND_FACING : GROUND_FACING)) {
					// Ground only near the landscape: a roof or a canopy is not
					if (modelClass != ModelClass::Terrain && overLand && cz > landHeight + GROUND_WINDOW)
						continue;
					triangle.kind = Kind::Ground;
					triangle.steep = facing < GROUND_FACING;
				} else if (facing > LEAN_FACING)
					triangle.kind = Kind::Wall;
				else {
					triangle.kind = Kind::Underside;
					triangle.lean = facing > UNDERSIDE_FACING;
				}
				const float minX = std::min({ triangle.a[0], triangle.b[0], triangle.c[0] }), maxX = std::max({ triangle.a[0], triangle.b[0], triangle.c[0] });
				const float minY = std::min({ triangle.a[1], triangle.b[1], triangle.c[1] }), maxY = std::max({ triangle.a[1], triangle.b[1], triangle.c[1] });
				bounds.min[0] = std::min(bounds.min[0], minX);
				bounds.min[1] = std::min(bounds.min[1], minY);
				bounds.max[0] = std::max(bounds.max[0], maxX);
				bounds.max[1] = std::max(bounds.max[1], maxY);
				for (auto y = static_cast<int32_t>(std::floor(minY / BUCKET)); y <= static_cast<int32_t>(std::floor(maxY / BUCKET)); y++)
					for (auto x = static_cast<int32_t>(std::floor(minX / BUCKET)); x <= static_cast<int32_t>(std::floor(maxX / BUCKET)); x++)
						buckets[Key(x, y)].push_back(triangle);
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		if (bounds.min[0] <= bounds.max[0])
			changes.push_back(bounds);
	}

	GroundTiles::~GroundTiles()
	{
		Release();
	}

	void GroundTiles::Clear()
	{
		meshes.Clear();
		meshVersion = meshes.GetVersion();
		state = {};
		slots = {};
		uploads.clear();
		landCells = 0;
		landFilled.clear();
		navmesh.clear();
		navmeshRead = false;
		enteredAt = std::chrono::steady_clock::now();
		std::scoped_lock lock(mutex);
		jobs.clear();
		results.clear();
		generation++;
	}

	void GroundTiles::Release()
	{
		Clear();
		if (worker.joinable()) {
			worker.request_stop();
			worker.join();
		}
		texture = nullptr;
		srv = nullptr;
		maxTexture = nullptr;
		maxSRV = nullptr;
		landTexture = nullptr;
		landSRV = nullptr;
		worldspace.clear();
		cached.clear();
	}

	void GroundTiles::UpdateLand(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, float a_playerX, float a_playerY)
	{
		constexpr int32_t PER_CELL = static_cast<int32_t>(LandscapeCache::CELL_UNITS / LAND_TEXEL);
		if (!landTexture) {
			const D3D11_TEXTURE2D_DESC desc{ .Width = LAND_TEXELS, .Height = LAND_TEXELS, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R32G32_FLOAT, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = 0 };
			if (FAILED(a_device->CreateTexture2D(&desc, nullptr, landTexture.put())) || FAILED(a_device->CreateShaderResourceView(landTexture.get(), nullptr, landSRV.put()))) {
				landTexture = nullptr;
				landSRV = nullptr;
				return;
			}
			Util::SetResourceName(landTexture.get(), "FluidField::GroundLand");
			landFilled.clear();
		}
		landCell[0] = static_cast<int32_t>(std::floor(a_playerX / LandscapeCache::CELL_UNITS));
		landCell[1] = static_cast<int32_t>(std::floor(a_playerY / LandscapeCache::CELL_UNITS));
		// Cells left behind are forgotten (their texels are rewritten by whichever cell comes into their place)
		std::erase_if(landFilled, [&](uint64_t a_key) {
			const auto x = static_cast<int32_t>(a_key >> 32), y = static_cast<int32_t>(a_key & 0xFFFFFFFF);
			return std::abs(x - landCell[0]) > LAND_REACH || std::abs(y - landCell[1]) > LAND_REACH;
		});
		// One new loaded cell a frame, nearest first
		for (int32_t ring = 0; ring <= LAND_REACH; ring++) {
			for (int32_t cy = landCell[1] - ring; cy <= landCell[1] + ring; cy++) {
				for (int32_t cx = landCell[0] - ring; cx <= landCell[0] + ring; cx++) {
					if (std::max(std::abs(cx - landCell[0]), std::abs(cy - landCell[1])) != ring || landFilled.contains(Key(cx, cy)) || !a_landscape.Find(cx, cy))
						continue;
					landFilled.insert(Key(cx, cy));
					const float originX = static_cast<float>(cx * LandscapeCache::CELL_UNITS), originY = static_cast<float>(cy * LandscapeCache::CELL_UNITS);
					std::vector<WaterSurfaces::Plane> planes;
					a_water.CollectPlanes(originX, originY, originX + LandscapeCache::CELL_UNITS, originY + LandscapeCache::CELL_UNITS, planes);
					std::vector<float> texels(static_cast<std::size_t>(PER_CELL) * PER_CELL * 2);
					for (int32_t y = 0; y < PER_CELL; y++) {
						for (int32_t x = 0; x < PER_CELL; x++) {
							const float px = originX + (x + 0.5f) * LAND_TEXEL, py = originY + (y + 0.5f) * LAND_TEXEL;
							float height = NO_GROUND;
							a_landscape.SampleHeight(px, py, height);
							uint32_t flags = FOG_MAY_FORM;
							for (const auto& plane : planes) {
								if (px >= plane.minX && px <= plane.maxX && py >= plane.minY && py <= plane.maxY && plane.height > height) {
									height = plane.height;
									flags = WATER;
								}
							}
							texels[(static_cast<std::size_t>(y) * PER_CELL + x) * 2] = height;
							texels[(static_cast<std::size_t>(y) * PER_CELL + x) * 2 + 1] = static_cast<float>(flags);
						}
					}
					const auto left = static_cast<UINT>((cx * PER_CELL) & (LAND_TEXELS - 1)), top = static_cast<UINT>((cy * PER_CELL) & (LAND_TEXELS - 1));
					const D3D11_BOX box{ .left = left, .top = top, .front = 0, .right = left + PER_CELL, .bottom = top + PER_CELL, .back = 1 };
					a_context->UpdateSubresource(landTexture.get(), 0, &box, texels.data(), PER_CELL * 2 * sizeof(float), 0);
					return;
				}
			}
		}
	}

	std::size_t GroundTiles::GetQueued() const
	{
		std::scoped_lock lock(mutex);
		return jobs.size();
	}

	uint32_t GroundTiles::GetProvisional() const
	{
		return static_cast<uint32_t>(std::ranges::count_if(state, [](const Slot& a_slot) { return a_slot.built && !a_slot.final; }));
	}

	void GroundTiles::EnterWorldspace(const std::string& a_name)
	{
		worldspace = a_name;
		cached.clear();
		{
			std::scoped_lock lock(mutex);
			workerCacheDir = Util::PathHelpers::GetCommunityShaderPath() / "FogGround" / a_name / std::format("{:016x}", LoadOrderHash());
		}
	}

	void GroundTiles::Update(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, float a_playerX, float a_playerY, float a_deltaTime)
	{
		clock += a_deltaTime;
		// Item 9: indoors, the cell itself (its form ID): its tiles cached per cell, no landscape
		auto* tes = RE::TES::GetSingleton();
		const auto* interior = tes ? tes->interiorCell : nullptr;
		const auto* land = interior ? nullptr : GetLandWorldspace();
		const std::string name = interior ? std::format("Interior{:08X}", interior->GetFormID()) : land && land->GetFormEditorID() ? land->GetFormEditorID() :
		                                                                                                                             "";
		if (name.empty())
			return;
		if (name != worldspace) {
			Clear();
			EnterWorldspace(name);
		}
		if (!worker.joinable())
			worker = std::jthread([this](std::stop_token a_stop) { Run(a_stop); });
		if (!texture) {
			const D3D11_TEXTURE2D_DESC desc{ .Width = RESIDENT_TEXELS, .Height = RESIDENT_TEXELS, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = 0 };
			const D3D11_TEXTURE2D_DESC maxDesc{ .Width = SLOTS * MAX_PER_TILE, .Height = SLOTS * MAX_PER_TILE, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R32_FLOAT, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = 0 };
			if (FAILED(a_device->CreateTexture2D(&desc, nullptr, texture.put())) || FAILED(a_device->CreateShaderResourceView(texture.get(), nullptr, srv.put())) ||
				FAILED(a_device->CreateTexture2D(&maxDesc, nullptr, maxTexture.put())) || FAILED(a_device->CreateShaderResourceView(maxTexture.get(), nullptr, maxSRV.put()))) {
				texture = nullptr;
				srv = nullptr;
				maxTexture = nullptr;
				maxSRV = nullptr;
				return;
			}
			Util::SetResourceName(maxTexture.get(), "FluidField::GroundTilesMax");
			Util::SetResourceName(texture.get(), "FluidField::GroundTiles");
			Util::SetResourceName(srv.get(), "FluidField::GroundTiles SRV");
		}

		if (interior) {
			// Out of every reader's reach: no land map indoors
			landCell[0] = landCell[1] = 1 << 20;
			if (!navmeshRead && interior->IsAttached()) {
				navmeshRead = true;
				if (const auto* cellNavmeshes = interior->GetRuntimeData().navMeshes)
					for (const auto& mesh : cellNavmeshes->navMeshes) {
						if (!mesh)
							continue;
						const auto& vertices = mesh->vertices;
						for (const auto& source : mesh->triangles) {
							if (source.vertices[0] >= vertices.size() || source.vertices[1] >= vertices.size() || source.vertices[2] >= vertices.size())
								continue;
							GroundMeshes::Triangle triangle{};
							float* out[3] = { triangle.a, triangle.b, triangle.c };
							for (int k = 0; k < 3; k++) {
								const auto& location = vertices[source.vertices[k]].location;
								out[k][0] = location.x;
								out[k][1] = location.y;
								out[k][2] = location.z;
							}
							navmesh.push_back(triangle);
						}
					}
				logger::info("[Fluid Field] Ground fog indoors: {} navmesh triangles (fog forms only on the floor they cover)", navmesh.size());
			}
		} else
			UpdateLand(a_device, a_context, a_landscape, a_water, a_playerX, a_playerY);

		// The window: SLOTS tiles a side, the player in its middle two
		tileMin[0] = static_cast<int32_t>(std::floor(a_playerX / TILE_UNITS + 0.5f)) - SLOTS / 2;
		tileMin[1] = static_cast<int32_t>(std::floor(a_playerY / TILE_UNITS + 0.5f)) - SLOTS / 2;
		for (int32_t y = tileMin[1]; y < tileMin[1] + SLOTS; y++) {
			for (int32_t x = tileMin[0]; x < tileMin[0] + SLOTS; x++) {
				Slot& slot = SlotOf(state, x, y);
				if (slot.x != x || slot.y != y) {
					slot = Slot{ .x = x, .y = y };
					slots[&slot - state.data()].valid = 0;
				}
			}
		}
		const auto travelled = [&] {
			const float dx = a_playerX - playerPos[0], dy = a_playerY - playerPos[1];
			const float length = std::hypot(dx, dy);
			return length > 1.0f && length < TILE_UNITS ? std::array{ dx / length, dy / length } : std::array{ 0.0f, 0.0f };
		}();
		{
			std::scoped_lock lock(mutex);
			playerPos[0] = a_playerX;
			playerPos[1] = a_playerY;
			if (travelled[0] != 0.0f || travelled[1] != 0.0f) {
				travel[0] = travelled[0];
				travel[1] = travelled[1];
			}
			// Jobs for tiles that left the window are dropped
			std::erase_if(jobs, [&](const auto& a_job) { return a_job->x < tileMin[0] || a_job->x >= tileMin[0] + SLOTS || a_job->y < tileMin[1] || a_job->y >= tileMin[1] + SLOTS; });
		}
		const auto wanted = [&](int32_t a_x, int32_t a_y) -> Slot* {
			Slot& slot = SlotOf(state, a_x, a_y);
			return slot.x == a_x && slot.y == a_y ? &slot : nullptr;
		};

		// The statics' triangles: references over tiles already cached are not read
		const bool boost = std::chrono::steady_clock::now() - enteredAt < std::chrono::duration<double>(LOAD_BOOST_SECONDS);
		meshes.Update(a_landscape, a_playerX, a_playerY, SCAN_RADIUS, boost ? LOAD_READ_BUDGET_MS : READ_BUDGET_MS, [&](const GroundMeshes::Rect& a_rect) {
			for (auto y = static_cast<int32_t>(std::floor(a_rect.min[1] / TILE_UNITS)); y <= static_cast<int32_t>(std::floor(a_rect.max[1] / TILE_UNITS)); y++)
				for (auto x = static_cast<int32_t>(std::floor(a_rect.min[0] / TILE_UNITS)); x <= static_cast<int32_t>(std::floor(a_rect.max[0] / TILE_UNITS)); x++)
					if (!cached.contains(Key(x, y)))
						return false;
			return true;
		});
		std::vector<GroundMeshes::Rect> changed;
		meshes.ChangesSince(meshVersion, changed);
		meshVersion = meshes.GetVersion();
		for (const auto& rect : changed) {
			for (auto y = static_cast<int32_t>(std::floor((rect.min[1] - TEXEL) / TILE_UNITS)); y <= static_cast<int32_t>(std::floor((rect.max[1] + TEXEL) / TILE_UNITS)); y++)
				for (auto x = static_cast<int32_t>(std::floor((rect.min[0] - TEXEL) / TILE_UNITS)); x <= static_cast<int32_t>(std::floor((rect.max[0] + TEXEL) / TILE_UNITS)); x++)
					if (Slot* slot = wanted(x, y))
						slot->dirty = true;
		}
		if (a_landscape.GetCellCount() != landCells || a_water.GetSignature() != waterSignature) {
			landCells = a_landscape.GetCellCount();
			waterSignature = a_water.GetSignature();
			for (auto& slot : state)
				slot.dirty = true;
		}

		// Finished tiles, nearest first as the worker took them
		{
			std::scoped_lock lock(mutex);
			for (auto& result : results)
				uploads.push_back(std::move(result));
			results.clear();
		}
		int uploaded = 0;
		while (!uploads.empty() && uploaded < UPLOADS_PER_FRAME) {
			Result result = std::move(uploads.front());
			uploads.pop_front();
			Slot* slot = wanted(result.x, result.y);
			if (!slot)
				continue;
			slot->inFlight = false;
			if (result.miss) {
				slot->missed = true;
				continue;
			}
			const auto originX = static_cast<UINT>((result.x & (SLOTS - 1)) * TILE_TEXELS), originY = static_cast<UINT>((result.y & (SLOTS - 1)) * TILE_TEXELS);
			const D3D11_BOX box{ .left = originX, .top = originY, .front = 0, .right = originX + TILE_TEXELS, .bottom = originY + TILE_TEXELS, .back = 1 };
			a_context->UpdateSubresource(texture.get(), 0, &box, result.texels.data(), TILE_TEXELS * 4 * sizeof(float), 0);
			// Its highest top per MAX_TEXEL block
			constexpr uint32_t BLOCK = TILE_TEXELS / MAX_PER_TILE;
			std::array<float, MAX_PER_TILE * MAX_PER_TILE> highest;
			highest.fill(NO_GROUND);
			for (uint32_t y = 0; y < TILE_TEXELS; y++)
				for (uint32_t x = 0; x < TILE_TEXELS; x++) {
					float& block = highest[(y / BLOCK) * MAX_PER_TILE + x / BLOCK];
					block = std::max(block, result.texels[(static_cast<std::size_t>(y) * TILE_TEXELS + x) * 4]);
				}
			const auto maxX = static_cast<UINT>((result.x & (SLOTS - 1)) * MAX_PER_TILE), maxY = static_cast<UINT>((result.y & (SLOTS - 1)) * MAX_PER_TILE);
			const D3D11_BOX maxBox{ .left = maxX, .top = maxY, .front = 0, .right = maxX + MAX_PER_TILE, .bottom = maxY + MAX_PER_TILE, .back = 1 };
			a_context->UpdateSubresource(maxTexture.get(), 0, &maxBox, highest.data(), MAX_PER_TILE * sizeof(float), 0);
			slots[slot - state.data()] = { result.x, result.y, 1, 0 };
			slot->built = true;
			slot->final = result.settled;
			if (result.settled)
				cached.insert(Key(result.x, result.y));
			uploaded++;
		}

		// New jobs: a look in the disk cache for every new tile; builds (inputs copied here) for the nearest few that need one
		std::vector<std::unique_ptr<Job>> fresh;
		std::vector<std::pair<float, Slot*>> builds;
		for (auto& slot : state) {
			if (slot.inFlight || slot.x == INT32_MIN || (slot.built && slot.final))
				continue;
			if (!slot.built && !slot.missed) {
				fresh.push_back(std::make_unique<Job>(Job{ .x = slot.x, .y = slot.y, .loadOnly = true }));
				slot.inFlight = true;
				continue;
			}
			if (slot.built && (clock - slot.lastBuild <= REBUILD_SECONDS || clock < slot.nextCheck))
				continue;
			slot.nextCheck = clock + CHECK_SECONDS;
			// Settled: its landscape all there (and round it, for the statics reaching in), then a scan since and nothing left to read
			const GroundMeshes::Rect rect{ { slot.x * TILE_UNITS, slot.y * TILE_UNITS }, { (slot.x + 1) * TILE_UNITS, (slot.y + 1) * TILE_UNITS } };
			// Indoors no landscape: wait for the cell to load and its statics' 3D to stream in instead
			bool landThere = !interior || (interior->IsAttached() && std::chrono::steady_clock::now() - enteredAt > std::chrono::duration<double>(INTERIOR_SETTLE_SECONDS));
			if (!interior)
				for (auto cy = static_cast<int32_t>(std::floor((rect.min[1] - 512.0f) / LandscapeCache::CELL_UNITS)); cy <= static_cast<int32_t>(std::floor((rect.max[1] + 512.0f) / LandscapeCache::CELL_UNITS)); cy++)
					for (auto cx = static_cast<int32_t>(std::floor((rect.min[0] - 512.0f) / LandscapeCache::CELL_UNITS)); cx <= static_cast<int32_t>(std::floor((rect.max[0] + 512.0f) / LandscapeCache::CELL_UNITS)); cx++)
						landThere = landThere && a_landscape.Find(cx, cy);
			if (!landThere)
				slot.landScan = UINT32_MAX;
			else if (slot.landScan == UINT32_MAX)
				slot.landScan = meshes.GetScanCount();
			slot.settled = landThere && meshes.Settled(rect, slot.landScan);
			if (!slot.built || slot.dirty || slot.settled) {
				const float dx = (slot.x + 0.5f) * TILE_UNITS - a_playerX, dy = (slot.y + 0.5f) * TILE_UNITS - a_playerY;
				builds.emplace_back(std::hypot(dx, dy) - (dx * travel[0] + dy * travel[1]), &slot);
			}
		}
		std::ranges::sort(builds, {}, &std::pair<float, Slot*>::first);
		for (std::size_t i = 0; i < builds.size() && i < BUILDS_PER_FRAME; i++) {
			Slot& slot = *builds[i].second;
			auto job = std::make_unique<Job>(Job{ .x = slot.x, .y = slot.y, .loadOnly = false, .settled = slot.settled, .interior = interior != nullptr });
			const float min[2] = { slot.x * TILE_UNITS - TEXEL, slot.y * TILE_UNITS - TEXEL }, max[2] = { (slot.x + 1) * TILE_UNITS + TEXEL, (slot.y + 1) * TILE_UNITS + TEXEL };
			job->landscape = a_landscape.Subset(min[0], min[1], max[0], max[1]);
			a_water.CollectPlanes(min[0], min[1], max[0], max[1], job->water);
			meshes.Copy(min, max, job->triangles);
			for (const auto& triangle : navmesh)
				if (std::max({ triangle.a[0], triangle.b[0], triangle.c[0] }) >= min[0] && std::min({ triangle.a[0], triangle.b[0], triangle.c[0] }) <= max[0] &&
					std::max({ triangle.a[1], triangle.b[1], triangle.c[1] }) >= min[1] && std::min({ triangle.a[1], triangle.b[1], triangle.c[1] }) <= max[1])
					job->navmesh.push_back(triangle);
			fresh.push_back(std::move(job));
			slot.inFlight = true;
			slot.dirty = false;
			slot.lastBuild = clock;
		}
		if (!fresh.empty()) {
			{
				std::scoped_lock lock(mutex);
				for (auto& job : fresh)
					jobs.push_back(std::move(job));
			}
			wake.notify_one();
		}
	}

	void GroundTiles::Run(std::stop_token a_stop)
	{
		std::vector<float> texels;
		while (!a_stop.stop_requested()) {
			std::unique_ptr<Job> job;
			uint64_t jobGeneration = 0;
			std::filesystem::path path;
			{
				std::unique_lock lock(mutex);
				if (!wake.wait(lock, a_stop, [&] { return !jobs.empty(); }))
					return;
				// Nearest the player first, ahead of it before behind
				const auto score = [&](const std::unique_ptr<Job>& a_job) {
					const float dx = (a_job->x + 0.5f) * TILE_UNITS - playerPos[0], dy = (a_job->y + 0.5f) * TILE_UNITS - playerPos[1];
					return std::hypot(dx, dy) - (dx * travel[0] + dy * travel[1]) - (a_job->loadOnly ? 1e6f : 0.0f);
				};
				const auto best = std::ranges::min_element(jobs, {}, score);
				job = std::move(*best);
				jobs.erase(best);
				jobGeneration = generation;
				path = workerCacheDir / std::format("{}.{}.bin", job->x, job->y);
			}
			Result result{ .x = job->x, .y = job->y, .miss = false, .settled = job->settled || job->loadOnly };
			result.texels.resize(static_cast<std::size_t>(TILE_TEXELS) * TILE_TEXELS * 4);
			if (job->loadOnly) {
				std::ifstream file(path, std::ios::binary);
				uint32_t header[2]{};
				result.miss = !file || !file.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != CACHE_MAGIC || header[1] != CACHE_VERSION ||
				              !file.read(reinterpret_cast<char*>(result.texels.data()), static_cast<std::streamsize>(result.texels.size() * sizeof(float)));
			} else {
				Build(*job, result.texels);
				// A tile with no ground at all is never cached (an interior read before its room loaded)
				const bool anyGround = std::ranges::any_of(std::views::iota(std::size_t{ 0 }, result.texels.size() / 4), [&](std::size_t i) { return result.texels[i * 4] > NO_GROUND; });
				if (job->settled && anyGround) {
					std::error_code error;
					std::filesystem::create_directories(path.parent_path(), error);
					const auto temporary = std::filesystem::path(path).concat(".tmp");
					std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
					const uint32_t header[2] = { CACHE_MAGIC, CACHE_VERSION };
					if (file.write(reinterpret_cast<const char*>(header), sizeof(header)) &&
						file.write(reinterpret_cast<const char*>(result.texels.data()), static_cast<std::streamsize>(result.texels.size() * sizeof(float)))) {
						file.close();
						std::filesystem::rename(temporary, path, error);
					}
				}
			}
			std::scoped_lock lock(mutex);
			if (jobGeneration == generation)
				results.push_back(std::move(result));
		}
	}

	void GroundTiles::Build(const Job& a_job, std::vector<float>& o_texels)
	{
		// One texel of margin round the tile, so steepness at its edge sees its neighbours
		constexpr int32_t N = TILE_TEXELS + 2;
		const float originX = a_job.x * TILE_UNITS - TEXEL, originY = a_job.y * TILE_UNITS - TEXEL;
		// Per texel: the top surface; the landscape (or water) under everything; the highest underside under the top, and the
		// floor under that (D195)
		std::vector<float> heights(N * N, NO_GROUND), rock(N * N, NO_GROUND), land(N * N, NO_GROUND), under(N * N, NO_GROUND), lower(N * N, NO_GROUND), lowerRock(N * N, NO_GROUND);
		std::vector<uint8_t> flags(N * N, 0), steepTop(N * N, 0), lowerFlags(N * N, 0), steepLower(N * N, 0);
		for (int32_t y = 0; y < N; y++) {
			for (int32_t x = 0; x < N; x++) {
				const float px = originX + (x + 0.5f) * TEXEL, py = originY + (y + 0.5f) * TEXEL;
				float height = 0.0f;
				if (!a_job.landscape.SampleHeight(px, py, height))
					continue;
				const std::size_t i = static_cast<std::size_t>(y) * N + x;
				// Water over the land: its surface, no fog for now
				for (const auto& plane : a_job.water) {
					if (px >= plane.minX && px <= plane.maxX && py >= plane.minY && py <= plane.maxY && plane.height > height) {
						height = plane.height;
						flags[i] = WATER;
					}
				}
				heights[i] = land[i] = height;
				lowerFlags[i] = flags[i];
			}
		}
		// Every texel centre inside a triangle, with the triangle's height there
		const auto cover = [&](const GroundMeshes::Triangle& a_triangle, auto&& a_texel) {
			const float area = Area2(a_triangle.a, a_triangle.b, a_triangle.c);
			if (std::abs(area) < 1e-4f)
				return;
			const float minX = std::min({ a_triangle.a[0], a_triangle.b[0], a_triangle.c[0] }), maxX = std::max({ a_triangle.a[0], a_triangle.b[0], a_triangle.c[0] });
			const float minY = std::min({ a_triangle.a[1], a_triangle.b[1], a_triangle.c[1] }), maxY = std::max({ a_triangle.a[1], a_triangle.b[1], a_triangle.c[1] });
			const int32_t x0 = std::max(0, static_cast<int32_t>(std::ceil((minX - originX) / TEXEL - 0.5f))), x1 = std::min(N - 1, static_cast<int32_t>(std::floor((maxX - originX) / TEXEL - 0.5f)));
			const int32_t y0 = std::max(0, static_cast<int32_t>(std::ceil((minY - originY) / TEXEL - 0.5f))), y1 = std::min(N - 1, static_cast<int32_t>(std::floor((maxY - originY) / TEXEL - 0.5f)));
			for (int32_t y = y0; y <= y1; y++) {
				for (int32_t x = x0; x <= x1; x++) {
					const float p[2] = { originX + (x + 0.5f) * TEXEL, originY + (y + 0.5f) * TEXEL };
					const float wa = Area2(a_triangle.b, a_triangle.c, p) / area;
					const float wb = Area2(a_triangle.c, a_triangle.a, p) / area;
					const float wc = 1.0f - wa - wb;
					const std::size_t i = static_cast<std::size_t>(y) * N + x;
					// Outdoors only over the landscape (what hangs past it is not ground); indoors there is none (item 9)
					if (wa >= 0.0f && wb >= 0.0f && wc >= 0.0f && (land[i] > NO_GROUND || a_job.interior))
						a_texel(i, wa * a_triangle.a[2] + wb * a_triangle.b[2] + wc * a_triangle.c[2]);
				}
			}
		};
		// The top: the highest ground, and the highest rock
		for (const auto& triangle : a_job.triangles) {
			if (triangle.kind == GroundMeshes::Kind::Underside)
				continue;
			cover(triangle, [&](std::size_t i, float z) {
				if (triangle.kind == GroundMeshes::Kind::Wall)
					rock[i] = std::max(rock[i], z);
				else if (z > heights[i]) {
					heights[i] = z;
					steepTop[i] = triangle.steep;
					flags[i] &= ~WATER;
				}
			});
		}
		// An overhang: the highest underside under the top, clear of the land; then the highest surface well under it
		for (const auto& triangle : a_job.triangles) {
			if (triangle.kind == GroundMeshes::Kind::Underside)
				cover(triangle, [&](std::size_t i, float z) {
					const float top = std::max(heights[i], rock[i]);
					if (z < top - 1.0f && z > land[i] + OVERHANG_CLEARANCE && !(triangle.lean && top - z > LIP_THICKNESS))
						under[i] = std::max(under[i], z);
				});
		}
		for (int pass = 0; pass < UNDERSIDE_FILL; pass++) {
			std::vector<float> filled(under);
			for (int32_t y = 1; y < N - 1; y++) {
				for (int32_t x = 1; x < N - 1; x++) {
					const std::size_t i = static_cast<std::size_t>(y) * N + x;
					const float top = std::max(heights[i], rock[i]);
					if (under[i] > NO_GROUND || land[i] <= NO_GROUND || top < land[i] + 2.0f * OVERHANG_CLEARANCE)
						continue;
					for (int32_t dy = -1; dy <= 1; dy++)
						for (int32_t dx = -1; dx <= 1; dx++) {
							const float z = under[static_cast<std::size_t>(y + dy) * N + (x + dx)];
							if (z > NO_GROUND && z < top - 1.0f && z > land[i] + OVERHANG_CLEARANCE)
								filled[i] = std::max(filled[i], z);
						}
				}
			}
			under.swap(filled);
		}
		for (std::size_t i = 0; i < under.size(); i++)
			if (under[i] > NO_GROUND)
				lower[i] = land[i];
		for (const auto& triangle : a_job.triangles) {
			if (triangle.kind == GroundMeshes::Kind::Underside)
				continue;
			cover(triangle, [&](std::size_t i, float z) {
				if (under[i] <= NO_GROUND || z > under[i] - OVERHANG_CLEARANCE)
					return;
				if (triangle.kind == GroundMeshes::Kind::Wall && !triangle.flat)
					lowerRock[i] = std::max(lowerRock[i], z);
				else if (z > lower[i]) {
					lower[i] = z;
					steepLower[i] = triangle.steep;
					lowerFlags[i] &= ~WATER;
				}
			});
		}
		// Rock standing over the ground is the level's top
		for (std::size_t i = 0; i < heights.size(); i++) {
			if (heights[i] > NO_GROUND && rock[i] > heights[i] + ROCK_RISE) {
				heights[i] = rock[i];
				flags[i] = ROCK;
			}
			if (lower[i] > NO_GROUND && lowerRock[i] > lower[i] + ROCK_RISE) {
				lower[i] = lowerRock[i];
				lowerFlags[i] = ROCK;
			}
		}
		// Indoors with a navmesh (item 9, Victor's single plane): each column's lowest navmesh floor, widened into the gaps the
		// navmesh leaves at walls and props (on a grid NAVMESH_WIDEN texels larger each side, so tiles agree at their seams)
		const bool navmeshed = a_job.interior && !a_job.navmesh.empty();
		std::vector<float> navFloor;
		if (navmeshed) {
			constexpr int32_t W = NAVMESH_WIDEN;
			const int32_t M = N + 2 * W;
			const float gridX = originX - W * TEXEL, gridY = originY - W * TEXEL;
			constexpr float NONE = std::numeric_limits<float>::max();
			std::vector<float> grid(static_cast<std::size_t>(M) * M, NONE);
			for (const auto& triangle : a_job.navmesh) {
				const float area = Area2(triangle.a, triangle.b, triangle.c);
				if (std::abs(area) < 1e-4f)
					continue;
				const int32_t x0 = std::max(0, static_cast<int32_t>(std::ceil((std::min({ triangle.a[0], triangle.b[0], triangle.c[0] }) - gridX) / TEXEL - 0.5f)));
				const int32_t x1 = std::min(M - 1, static_cast<int32_t>(std::floor((std::max({ triangle.a[0], triangle.b[0], triangle.c[0] }) - gridX) / TEXEL - 0.5f)));
				const int32_t y0 = std::max(0, static_cast<int32_t>(std::ceil((std::min({ triangle.a[1], triangle.b[1], triangle.c[1] }) - gridY) / TEXEL - 0.5f)));
				const int32_t y1 = std::min(M - 1, static_cast<int32_t>(std::floor((std::max({ triangle.a[1], triangle.b[1], triangle.c[1] }) - gridY) / TEXEL - 0.5f)));
				for (int32_t y = y0; y <= y1; y++)
					for (int32_t x = x0; x <= x1; x++) {
						const float p[2] = { gridX + (x + 0.5f) * TEXEL, gridY + (y + 0.5f) * TEXEL };
						const float wa = Area2(triangle.b, triangle.c, p) / area, wb = Area2(triangle.c, triangle.a, p) / area, wc = 1.0f - wa - wb;
						if (wa < 0.0f || wb < 0.0f || wc < 0.0f)
							continue;
						float& cell = grid[static_cast<std::size_t>(y) * M + x];
						cell = std::min(cell, wa * triangle.a[2] + wb * triangle.b[2] + wc * triangle.c[2]);
					}
			}
			for (int pass = 0; pass < W; pass++) {
				std::vector<float> widened(grid);
				for (int32_t y = 1; y < M - 1; y++)
					for (int32_t x = 1; x < M - 1; x++) {
						float& cell = widened[static_cast<std::size_t>(y) * M + x];
						if (cell != NONE)
							continue;
						for (int32_t dy = -1; dy <= 1; dy++)
							for (int32_t dx = -1; dx <= 1; dx++)
								cell = std::min(cell, grid[static_cast<std::size_t>(y + dy) * M + (x + dx)]);
					}
				grid.swap(widened);
			}
			navFloor.assign(heights.size(), NO_GROUND);
			for (int32_t y = 0; y < N; y++)
				for (int32_t x = 0; x < N; x++) {
					const float z = grid[static_cast<std::size_t>(y + W) * M + (x + W)];
					navFloor[static_cast<std::size_t>(y) * N + x] = z == NONE ? NO_GROUND : z;
				}
		}
		// Indoors, a rock top level with ground beside it is a step's edge: ground. Outdoors left as tuned
		// shortcut: indoors only; outdoor ledges may want it too, test them before widening it
		if (a_job.interior) {
			const std::vector<uint8_t> rockFlags(flags);
			for (int32_t y = 1; y < N - 1; y++)
				for (int32_t x = 1; x < N - 1; x++) {
					const std::size_t i = static_cast<std::size_t>(y) * N + x;
					if (!(rockFlags[i] & ROCK))
						continue;
					bool edge = false;
					for (int32_t dy = -1; dy <= 1 && !edge; dy++)
						for (int32_t dx = -1; dx <= 1 && !edge; dx++) {
							const std::size_t j = static_cast<std::size_t>(y + dy) * N + (x + dx);
							edge = !(rockFlags[j] & ROCK) && heights[j] > NO_GROUND && std::abs(heights[j] - heights[i]) <= STEP_EDGE;
						}
					if (edge) {
						flags[i] = 0;
						steepTop[i] = 0;
					}
				}
		}
		// Steep: rising on both sides along an axis (a slope, not a ledge's single step), or a steep landscape mesh face
		const auto finish = [&](const std::vector<float>& a_heights, std::size_t i, uint32_t a_flag, bool a_steepFace) {
			for (const std::size_t step : { std::size_t{ 1 }, static_cast<std::size_t>(N) }) {
				const float before = a_heights[i] - a_heights[i - step], after = a_heights[i + step] - a_heights[i];
				if (a_heights[i - step] > NO_GROUND && a_heights[i + step] > NO_GROUND && before * after > 0.0f && std::min(std::abs(before), std::abs(after)) > STEEP_RISE)
					a_flag |= STEEP;
			}
			if (a_steepFace && !(a_flag & ROCK))
				a_flag |= STEEP;
			if (!(a_flag & (ROCK | STEEP | WATER)))
				a_flag |= FOG_MAY_FORM;
			if (!a_job.settled)
				a_flag |= PROVISIONAL;
			return static_cast<float>(a_flag);
		};
		if (navmeshed) {
			// One level: the navmesh's floor, at a mesh floor's exact height where one lies near it
			std::vector<float> plane(navFloor);
			for (std::size_t i = 0; i < plane.size(); i++) {
				if (plane[i] <= NO_GROUND)
					continue;
				float nearest = NAVMESH_REACH;
				for (const auto& [height, flag] : { std::pair{ heights[i], flags[i] }, std::pair{ lower[i], lowerFlags[i] } })
					if (height > NO_GROUND && !(flag & ROCK) && std::abs(height - navFloor[i]) < nearest) {
						nearest = std::abs(height - navFloor[i]);
						plane[i] = height;
					}
			}
			for (int32_t y = 0; y < static_cast<int32_t>(TILE_TEXELS); y++)
				for (int32_t x = 0; x < static_cast<int32_t>(TILE_TEXELS); x++) {
					const std::size_t i = static_cast<std::size_t>(y + 1) * N + (x + 1);
					float* out = &o_texels[(static_cast<std::size_t>(y) * TILE_TEXELS + x) * 4];
					out[0] = plane[i];
					out[1] = plane[i] > NO_GROUND ? finish(plane, i, 0, false) : 0.0f;
					out[2] = NO_GROUND;
					out[3] = 0.0f;
				}
			return;
		}
		// The floors' slopes against their neighbours' floors, or their tops where those have none
		std::vector<float> floors(lower);
		for (std::size_t i = 0; i < floors.size(); i++)
			if (floors[i] <= NO_GROUND)
				floors[i] = heights[i];
		for (int32_t y = 0; y < static_cast<int32_t>(TILE_TEXELS); y++) {
			for (int32_t x = 0; x < static_cast<int32_t>(TILE_TEXELS); x++) {
				const std::size_t i = static_cast<std::size_t>(y + 1) * N + (x + 1);
				float* out = &o_texels[(static_cast<std::size_t>(y) * TILE_TEXELS + x) * 4];
				out[0] = heights[i];
				out[1] = heights[i] > NO_GROUND ? finish(heights, i, flags[i], steepTop[i]) : 0.0f;
				// The gap up to the underside, for the draw's ceiling; a gap too low to hold fog is solid
				const float gap = under[i] - lower[i];
				const bool open = lower[i] > NO_GROUND && gap >= LEAST_GAP;
				out[2] = open ? lower[i] : NO_GROUND;
				out[3] = open ? finish(floors, i, lowerFlags[i], steepLower[i]) + GAP_SCALE * std::floor(std::min(gap, 4000.0f)) : 0.0f;
			}
		}
	}
}
