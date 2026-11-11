#include "GroundHeightMap.h"

#include "LandscapeCache.h"
#include "WaterSurfaces.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Model folders left out: they neither carry the fog nor stop it. */
		constexpr std::array<std::string_view, 7> IGNORED = { "plants\\", "\\plants\\", "clutter\\", "effects\\", "marker", "critters\\", "fxwaterfall" };
		/** @brief Model folders whose every triangle is a wall: things standing on the ground, not ground. */
		constexpr std::array<std::string_view, 5> WALLS = { "\\rocks\\", "\\trees\\", "\\logs\\", "\\stumps\\", "treelog" };

		/** @brief A triangle facing up at least this much is ground, down at least this much an underside; between, a wall. */
		constexpr float GROUND_FACING = 0.5f;
		constexpr float UNDERSIDE_FACING = -0.3f;

		enum class ModelClass
		{
			Ignored,
			Walls,
			ByFacing
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
			return base->Is(RE::FormType::Tree) || std::ranges::any_of(WALLS, has) ? ModelClass::Walls : ModelClass::ByFacing;
		}

		/** @brief The cross product's z of two edges: twice the triangle's signed area seen from above. */
		float Area2(const float* a_a, const float* a_b, const float* a_c)
		{
			return (a_b[0] - a_a[0]) * (a_c[1] - a_a[1]) - (a_b[1] - a_a[1]) * (a_c[0] - a_a[0]);
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
	}

	void GroundMeshes::ChangesSince(uint64_t a_version, std::vector<Rect>& o_rects) const
	{
		const uint64_t from = a_version > dropped ? a_version - dropped : 0;
		for (uint64_t i = from; i < changes.size(); i++)
			o_rects.push_back(changes[i]);
	}

	void GroundMeshes::Update(const LandscapeCache& a_landscape, float a_centreX, float a_centreY, float a_radius)
	{
		auto* tes = RE::TES::GetSingleton();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!tes || !player || tes->interiorCell)
			return;
		// Look for new references every so often (and at once when the queue ran dry after a load)
		if (framesToScan == 0 || framesToScan-- == 0) {
			framesToScan = SCAN_INTERVAL;
			tes->ForEachReferenceInRange(player, a_radius, [&](RE::TESObjectREFR* a_ref) {
				if (a_ref && seen.try_emplace(a_ref->GetFormID(), a_ref->GetPositionX(), a_ref->GetPositionY()).second)
					queue.push_back(a_ref->GetHandle());
				return RE::BSContainer::ForEachResult::kContinue;
			});
			// Buckets far behind the player are dropped
			const float keep = a_radius + 2.0f * BUCKET;
			std::erase_if(buckets, [&](const auto& a_bucket) {
				const float x = (static_cast<float>(static_cast<int32_t>(a_bucket.first >> 32)) + 0.5f) * BUCKET;
				const float y = (static_cast<float>(static_cast<int32_t>(a_bucket.first & 0xFFFFFFFF)) + 0.5f) * BUCKET;
				return std::abs(x - a_centreX) > keep || std::abs(y - a_centreY) > keep;
			});
			// and the references read there, so they are read again on the way back
			std::erase_if(seen, [&](const auto& a_seen) { return std::abs(a_seen.second.first - a_centreX) > a_radius + BUCKET || std::abs(a_seen.second.second - a_centreY) > a_radius + BUCKET; });
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(READ_BUDGET_MS));
		while (!queue.empty() && std::chrono::steady_clock::now() < deadline) {
			const auto handle = queue.front();
			queue.pop_front();
			if (const auto ref = handle.get(); ref && ref->Is3DLoaded())
				Read(*ref, a_landscape);
		}
		// The log of changed rectangles keeps its last few thousand (a consumer further behind fills its whole grid again)
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
				if (modelClass == ModelClass::Walls)
					triangle.kind = Kind::Wall;
				else if (facing >= GROUND_FACING) {
					// Ground only near the landscape: a roof or a canopy is not
					const float cx = (triangle.a[0] + triangle.b[0] + triangle.c[0]) / 3.0f, cy = (triangle.a[1] + triangle.b[1] + triangle.c[1]) / 3.0f;
					const float cz = (triangle.a[2] + triangle.b[2] + triangle.c[2]) / 3.0f;
					float landHeight = 0.0f;
					if (a_landscape.SampleHeight(cx, cy, landHeight) && cz > landHeight + GROUND_WINDOW)
						continue;
					triangle.kind = Kind::Ground;
				} else if (facing <= UNDERSIDE_FACING)
					triangle.kind = Kind::Underside;
				else
					triangle.kind = Kind::Wall;
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

	void GroundHeightMap::Release()
	{
		texture = nullptr;
		srv = nullptr;
		heights.clear();
		objects.clear();
		lowers.clear();
		upload.clear();
		land.clear();
		rasterQueue.clear();
		resolution = 0;
		filled = false;
		maxHeight = NO_GROUND;
	}

	void GroundHeightMap::Queue(int32_t a_minX, int32_t a_minY, int32_t a_maxX, int32_t a_maxY, const int32_t a_gridMin[2])
	{
		const int32_t n = static_cast<int32_t>(resolution);
		const int32_t minX = std::max(a_minX, a_gridMin[0]), maxX = std::min(a_maxX, a_gridMin[0] + n - 1);
		const int32_t minY = std::max(a_minY, a_gridMin[1]), maxY = std::min(a_maxY, a_gridMin[1] + n - 1);
		for (int32_t y = minY; y <= maxY; y += BAND_ROWS)
			rasterQueue.push_back({ { minX, y }, { maxX, std::min(y + BAND_ROWS - 1, maxY) } });
	}

	void GroundHeightMap::Rasterize(const CellRect& a_rect, const GroundMeshes& a_meshes)
	{
		const int32_t n = static_cast<int32_t>(resolution);
		const int32_t mask = n - 1;
		const int32_t width = a_rect.max[0] - a_rect.min[0] + 1, height = a_rect.max[1] - a_rect.min[1] + 1;
		const auto indexOf = [&](int32_t a_x, int32_t a_y) { return static_cast<std::size_t>(a_y & mask) * n + (a_x & mask); };
		// From the landscape up
		for (int32_t y = a_rect.min[1]; y <= a_rect.max[1]; y++) {
			for (int32_t x = a_rect.min[0]; x <= a_rect.max[0]; x++) {
				const std::size_t i = indexOf(x, y);
				heights[i] = land[i];
				objects[i] = NO_GROUND;
				lowers[i] = NO_GROUND;
			}
		}
		scratch.assign(static_cast<std::size_t>(width) * height, NO_GROUND);
		const float worldMin[2] = { static_cast<float>(a_rect.min[0]) * cellSize, static_cast<float>(a_rect.min[1]) * cellSize };
		const float worldMax[2] = { static_cast<float>(a_rect.max[0] + 1) * cellSize, static_cast<float>(a_rect.max[1] + 1) * cellSize };

		// Every texel centre of the rectangle inside a triangle, with the triangle's height there
		const auto cover = [&](const GroundMeshes::Triangle& a_triangle, auto&& a_texel) {
			const float area = Area2(a_triangle.a, a_triangle.b, a_triangle.c);
			if (std::abs(area) < 1e-4f)
				return;
			const float minX = std::min({ a_triangle.a[0], a_triangle.b[0], a_triangle.c[0] }), maxX = std::max({ a_triangle.a[0], a_triangle.b[0], a_triangle.c[0] });
			const float minY = std::min({ a_triangle.a[1], a_triangle.b[1], a_triangle.c[1] }), maxY = std::max({ a_triangle.a[1], a_triangle.b[1], a_triangle.c[1] });
			const int32_t x0 = std::max(a_rect.min[0], static_cast<int32_t>(std::ceil(minX / cellSize - 0.5f))), x1 = std::min(a_rect.max[0], static_cast<int32_t>(std::floor(maxX / cellSize - 0.5f)));
			const int32_t y0 = std::max(a_rect.min[1], static_cast<int32_t>(std::ceil(minY / cellSize - 0.5f))), y1 = std::min(a_rect.max[1], static_cast<int32_t>(std::floor(maxY / cellSize - 0.5f)));
			for (int32_t y = y0; y <= y1; y++) {
				for (int32_t x = x0; x <= x1; x++) {
					const float p[2] = { (static_cast<float>(x) + 0.5f) * cellSize, (static_cast<float>(y) + 0.5f) * cellSize };
					const float wa = Area2(a_triangle.b, a_triangle.c, p) / area;
					const float wb = Area2(a_triangle.c, a_triangle.a, p) / area;
					const float wc = 1.0f - wa - wb;
					if (wa < 0.0f || wb < 0.0f || wc < 0.0f)
						continue;
					a_texel(x, y, wa * a_triangle.a[2] + wb * a_triangle.b[2] + wc * a_triangle.c[2]);
				}
			}
		};
		const auto known = [&](std::size_t a_index) { return land[a_index] > NO_GROUND; };

		// The ground: its highest surface; walls: their highest top
		a_meshes.ForEach(worldMin, worldMax, [&](const GroundMeshes::Triangle& a_triangle) {
			if (a_triangle.kind == GroundMeshes::Kind::Underside)
				return;
			cover(a_triangle, [&](int32_t x, int32_t y, float z) {
				const std::size_t i = indexOf(x, y);
				if (!known(i))
					return;
				if (a_triangle.kind == GroundMeshes::Kind::Ground)
					heights[i] = std::max(heights[i], z);
				else
					objects[i] = std::max(objects[i], z);
			});
		});
		// An overhang: the highest underside under the ground, then the highest surface well under that
		a_meshes.ForEach(worldMin, worldMax, [&](const GroundMeshes::Triangle& a_triangle) {
			if (a_triangle.kind != GroundMeshes::Kind::Underside)
				return;
			cover(a_triangle, [&](int32_t x, int32_t y, float z) {
				const std::size_t i = indexOf(x, y);
				float& under = scratch[static_cast<std::size_t>(y - a_rect.min[1]) * width + (x - a_rect.min[0])];
				if (known(i) && z < heights[i] - 1.0f && z > land[i] + OVERHANG_CLEARANCE)
					under = std::max(under, z);
			});
		});
		for (int32_t y = a_rect.min[1]; y <= a_rect.max[1]; y++) {
			for (int32_t x = a_rect.min[0]; x <= a_rect.max[0]; x++) {
				if (scratch[static_cast<std::size_t>(y - a_rect.min[1]) * width + (x - a_rect.min[0])] > NO_GROUND)
					lowers[indexOf(x, y)] = land[indexOf(x, y)];
			}
		}
		a_meshes.ForEach(worldMin, worldMax, [&](const GroundMeshes::Triangle& a_triangle) {
			if (a_triangle.kind != GroundMeshes::Kind::Ground)
				return;
			cover(a_triangle, [&](int32_t x, int32_t y, float z) {
				const std::size_t i = indexOf(x, y);
				const float under = scratch[static_cast<std::size_t>(y - a_rect.min[1]) * width + (x - a_rect.min[0])];
				if (under > NO_GROUND && z < under - OVERHANG_CLEARANCE)
					lowers[i] = std::max(lowers[i], z);
			});
		});
		for (int32_t y = a_rect.min[1]; y <= a_rect.max[1]; y++)
			MarkDirty(indexOf(a_rect.min[0], y));
	}

	bool GroundHeightMap::Update(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const GroundMeshes& a_meshes,
		const int32_t a_gridMin[2], uint32_t a_resolution, float a_cellSize)
	{
		if (a_resolution != resolution || a_cellSize != cellSize || !texture) {
			Release();
			D3D11_TEXTURE2D_DESC desc{
				.Width = a_resolution,
				.Height = a_resolution,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
				.SampleDesc = { 1, 0 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE,
				.CPUAccessFlags = 0,
				.MiscFlags = 0
			};
			if (FAILED(a_device->CreateTexture2D(&desc, nullptr, texture.put())) || FAILED(a_device->CreateShaderResourceView(texture.get(), nullptr, srv.put()))) {
				Release();
				return false;
			}
			Util::SetResourceName(texture.get(), "FluidField::GroundHeight");
			Util::SetResourceName(srv.get(), "FluidField::GroundHeight SRV");
			resolution = a_resolution;
			cellSize = a_cellSize;
			heights.assign(static_cast<std::size_t>(resolution) * resolution, NO_GROUND);
			land.assign(heights.size(), NO_GROUND);
			objects.assign(heights.size(), NO_GROUND);
			lowers.assign(heights.size(), NO_GROUND);
			rowDirty.assign(resolution, 1);
			meshVersion = a_meshes.GetVersion();
		}

		const int32_t n = static_cast<int32_t>(resolution);
		const int32_t mask = n - 1;
		const bool retry = updatesUntilRetry == 0;
		updatesUntilRetry = retry ? RETRY_INTERVAL : updatesUntilRetry - 1;

		// The landscape (or water) under cells new since the last fill (all, the first time) and, on a retry, those still over
		// unloaded land; then the meshes over the new strips
		int32_t retriedMin[2] = { INT32_MAX, INT32_MAX }, retriedMax[2] = { INT32_MIN, INT32_MIN };
		for (int32_t y = 0; y < n; y++) {
			const int32_t cellY = a_gridMin[1] + y;
			const bool rowWasInGrid = filled && cellY >= filledMin[1] && cellY < filledMin[1] + n;
			for (int32_t x = 0; x < n; x++) {
				const int32_t cellX = a_gridMin[0] + x;
				// Rows already in the grid: skip straight past the columns that were too, unless retrying
				if (rowWasInGrid && !retry && cellX >= filledMin[0] && cellX < filledMin[0] + n) {
					x = filledMin[0] + n - a_gridMin[0] - 1;
					continue;
				}
				const bool wasInGrid = rowWasInGrid && cellX >= filledMin[0] && cellX < filledMin[0] + n;
				const std::size_t index = static_cast<std::size_t>(cellY & mask) * n + (cellX & mask);
				if (wasInGrid && !(retry && land[index] == NO_GROUND))
					continue;
				float ground = 0.0f;
				const float worldX = (static_cast<float>(cellX) + 0.5f) * cellSize;
				const float worldY = (static_cast<float>(cellY) + 0.5f) * cellSize;
				float value = a_landscape.SampleHeight(worldX, worldY, ground) ? ground : NO_GROUND;
				float surface = 0.0f;
				if (value != NO_GROUND && a_water.SampleHeight(worldX, worldY, surface) != WaterSurfaces::Source::None && surface > value)
					value = FOG_ON_WATER ? surface : ON_WATER;
				if (wasInGrid && value == land[index])
					continue;
				land[index] = value;
				heights[index] = value;
				objects[index] = NO_GROUND;
				lowers[index] = NO_GROUND;
				MarkDirty(index);
				if (wasInGrid) {
					retriedMin[0] = std::min(retriedMin[0], cellX);
					retriedMin[1] = std::min(retriedMin[1], cellY);
					retriedMax[0] = std::max(retriedMax[0], cellX);
					retriedMax[1] = std::max(retriedMax[1], cellY);
				}
			}
		}
		if (!filled || std::abs(a_gridMin[0] - filledMin[0]) >= n || std::abs(a_gridMin[1] - filledMin[1]) >= n) {
			rasterQueue.clear();
			Queue(a_gridMin[0], a_gridMin[1], a_gridMin[0] + n - 1, a_gridMin[1] + n - 1, a_gridMin);
		} else {
			// The new rows across the whole width, and the new columns down the rows kept
			const int32_t oldMin[2] = { filledMin[0], filledMin[1] }, oldMax[2] = { filledMin[0] + n - 1, filledMin[1] + n - 1 };
			const int32_t newMax[2] = { a_gridMin[0] + n - 1, a_gridMin[1] + n - 1 };
			if (a_gridMin[1] < oldMin[1])
				Queue(a_gridMin[0], a_gridMin[1], newMax[0], oldMin[1] - 1, a_gridMin);
			if (newMax[1] > oldMax[1])
				Queue(a_gridMin[0], oldMax[1] + 1, newMax[0], newMax[1], a_gridMin);
			const int32_t keptMin = std::max(a_gridMin[1], oldMin[1]), keptMax = std::min(newMax[1], oldMax[1]);
			if (a_gridMin[0] < oldMin[0])
				Queue(a_gridMin[0], keptMin, oldMin[0] - 1, keptMax, a_gridMin);
			if (newMax[0] > oldMax[0])
				Queue(oldMax[0] + 1, keptMin, newMax[0], keptMax, a_gridMin);
		}
		if (retriedMin[0] <= retriedMax[0])
			Queue(retriedMin[0], retriedMin[1], retriedMax[0], retriedMax[1], a_gridMin);
		filledMin[0] = a_gridMin[0];
		filledMin[1] = a_gridMin[1];
		filled = true;

		// Meshes read in since the last update: their rectangles again
		std::vector<GroundMeshes::Rect> changed;
		a_meshes.ChangesSince(meshVersion, changed);
		meshVersion = a_meshes.GetVersion();
		for (const auto& rect : changed) {
			Queue(static_cast<int32_t>(std::floor(rect.min[0] / cellSize)), static_cast<int32_t>(std::floor(rect.min[1] / cellSize)),
				static_cast<int32_t>(std::floor(rect.max[0] / cellSize)), static_cast<int32_t>(std::floor(rect.max[1] / cellSize)), a_gridMin);
		}

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(RASTER_BUDGET_MS));
		while (!rasterQueue.empty() && std::chrono::steady_clock::now() < deadline) {
			CellRect rect = rasterQueue.front();
			rasterQueue.pop_front();
			// Clipped to where the grid is now (it may have moved since this was queued)
			rect.min[0] = std::max(rect.min[0], a_gridMin[0]);
			rect.min[1] = std::max(rect.min[1], a_gridMin[1]);
			rect.max[0] = std::min(rect.max[0], a_gridMin[0] + n - 1);
			rect.max[1] = std::min(rect.max[1], a_gridMin[1] + n - 1);
			if (rect.min[0] <= rect.max[0] && rect.min[1] <= rect.max[1])
				Rasterize(rect, a_meshes);
		}

		// Only the rows that changed (and their neighbours, whose steepness they change) are rebuilt and uploaded
		for (int32_t y = 0; y < n; y++) {
			if (!rowDirty[y])
				continue;
			int32_t last = y;
			while (last + 1 < n && rowDirty[last + 1])
				last++;
			upload.resize(static_cast<std::size_t>(last - y + 1) * n * 4);
			for (int32_t row = y; row <= last; row++) {
				for (int32_t x = 0; x < n; x++) {
					const std::size_t i = static_cast<std::size_t>(row) * n + x;
					// D193: how steep the ground is there (its largest rise to a neighbour): fog does not cling to a cliff's face
					float steep = 0.0f;
					if (heights[i] > NO_GROUND) {
						maxHeight = std::max(maxHeight, heights[i]);
						for (const auto [dx, dy] : { std::pair{ 1, 0 }, std::pair{ -1, 0 }, std::pair{ 0, 1 }, std::pair{ 0, -1 } }) {
							const float other = heights[static_cast<std::size_t>((row + dy) & mask) * n + ((x + dx) & mask)];
							if (other > NO_GROUND)
								steep = std::max(steep, std::abs(other - heights[i]));
						}
					}
					float* texel = &upload[(static_cast<std::size_t>(row - y) * n + x) * 4];
					texel[0] = heights[i];
					texel[1] = objects[i];
					texel[2] = lowers[i];
					texel[3] = steep;
				}
			}
			const D3D11_BOX box{ .left = 0, .top = static_cast<UINT>(y), .front = 0, .right = static_cast<UINT>(n), .bottom = static_cast<UINT>(last + 1), .back = 1 };
			a_context->UpdateSubresource(texture.get(), 0, &box, upload.data(), static_cast<UINT>(n * 4 * sizeof(float)), 0);
			std::fill(rowDirty.begin() + y, rowDirty.begin() + last + 1, static_cast<uint8_t>(0));
			y = last;
		}
		return true;
	}

	void GroundHeightMap::MarkDirty(std::size_t a_index)
	{
		const int32_t n = static_cast<int32_t>(resolution);
		const int32_t row = static_cast<int32_t>(a_index / resolution);
		rowDirty[(row + n - 1) & (n - 1)] = rowDirty[row] = rowDirty[(row + 1) & (n - 1)] = 1;
	}
}
