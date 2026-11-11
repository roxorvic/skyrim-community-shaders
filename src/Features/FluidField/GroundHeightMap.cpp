#include "GroundHeightMap.h"

#include "LandscapeCache.h"
#include "WaterSurfaces.h"

#include <algorithm>
#include <chrono>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief Rays continue past actors, clutter and trees (a trunk is not ground) this many times. */
		constexpr uint32_t MAX_STEP_THROUGHS = 4;
		constexpr float STEP_THROUGH_OFFSET = 2.0f;

		/** @brief Collision that is ground the fog lies on: the landscape and placed static meshes. */
		bool IsGroundLayer(RE::COL_LAYER a_layer)
		{
			switch (a_layer) {
			case RE::COL_LAYER::kStatic:
			case RE::COL_LAYER::kAnimStatic:
			case RE::COL_LAYER::kTerrain:
			case RE::COL_LAYER::kGround:
				return true;
			default:
				return false;
			}
		}

		/** @brief The highest ground hit straight down between two heights, or NO_GROUND. */
		float CastDown(RE::hkpWorld* a_world, float a_x, float a_y, float a_fromZ, float a_toZ)
		{
			const float scale = RE::bhkWorld::GetWorldScale();
			float fromZ = a_fromZ;
			for (uint32_t attempt = 0; attempt <= MAX_STEP_THROUGHS && fromZ > a_toZ; attempt++) {
				RE::hkpWorldRayCastInput input;
				input.from = RE::hkVector4(a_x * scale, a_y * scale, fromZ * scale, 0.0f);
				input.to = RE::hkVector4(a_x * scale, a_y * scale, a_toZ * scale, 0.0f);
				RE::hkpWorldRayCastOutput output;
				a_world->CastRay(input, output);
				if (!output.HasHit() || !output.rootCollidable)
					break;
				const float hitZ = fromZ + (a_toZ - fromZ) * output.hitFraction;
				if (IsGroundLayer(output.rootCollidable->GetCollisionLayer()))
					return hitZ;
				fromZ = hitZ - STEP_THROUGH_OFFSET;
			}
			return GroundHeightMap::NO_GROUND;
		}
	}

	void GroundHeightMap::Release()
	{
		texture = nullptr;
		srv = nullptr;
		heights.clear();
		land.clear();
		pending.clear();
		resolution = 0;
		filled = false;
		maxHeight = NO_GROUND;
	}

	bool GroundHeightMap::Update(ID3D11Device* a_device, ID3D11DeviceContext* a_context, const LandscapeCache& a_landscape, const WaterSurfaces& a_water, const int32_t a_gridMin[2],
		uint32_t a_resolution, float a_cellSize)
	{
		if (a_resolution != resolution || a_cellSize != cellSize || !texture) {
			Release();
			D3D11_TEXTURE2D_DESC desc{
				.Width = a_resolution,
				.Height = a_resolution,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = DXGI_FORMAT_R32_FLOAT,
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
		}

		const int32_t n = static_cast<int32_t>(resolution);
		const int32_t mask = n - 1;
		const bool retry = updatesUntilRetry == 0;
		updatesUntilRetry = retry ? RETRY_INTERVAL : updatesUntilRetry - 1;
		bool changed = false;

		// Sample cells new since the last fill (all, the first time) and, on a retry, those still over unloaded land
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
				float& height = heights[index];
				if (wasInGrid && !(retry && height == NO_GROUND))
					continue;
				float ground = 0.0f;
				const float worldX = (static_cast<float>(cellX) + 0.5f) * cellSize;
				const float worldY = (static_cast<float>(cellY) + 0.5f) * cellSize;
				float value = a_landscape.SampleHeight(worldX, worldY, ground) ? ground : NO_GROUND;
				land[index] = value;
				// Fog lies on water where there is water over the ground; elsewhere on what stands on the landscape (CastPending)
				float surface = 0.0f;
				if (value != NO_GROUND && a_water.SampleHeight(worldX, worldY, surface) != WaterSurfaces::Source::None && surface > value)
					value = surface;
				else if (value != NO_GROUND) {
					pending.emplace_back(cellX, cellY);
					pendingSorted = false;
				}
				if (value != height) {
					height = value;
					changed = true;
				}
			}
		}
		filledMin[0] = a_gridMin[0];
		filledMin[1] = a_gridMin[1];
		filled = true;
		changed |= CastPending(a_gridMin);

		if (changed) {
			maxHeight = *std::max_element(heights.begin(), heights.end());
			a_context->UpdateSubresource(texture.get(), 0, nullptr, heights.data(), static_cast<UINT>(resolution * sizeof(float)), 0);
		}
		return true;
	}

	bool GroundHeightMap::CastPending(const int32_t a_gridMin[2])
	{
		if (pending.empty())
			return false;
		const int32_t n = static_cast<int32_t>(resolution);
		const int32_t mask = n - 1;
		const int32_t centreX = a_gridMin[0] + n / 2, centreY = a_gridMin[1] + n / 2;
		if (!pendingSorted) {
			// Nearest last, so they are cast first; cells that scrolled out are dropped
			std::erase_if(pending, [&](const auto& a_cell) { return a_cell.first < a_gridMin[0] || a_cell.first >= a_gridMin[0] + n || a_cell.second < a_gridMin[1] || a_cell.second >= a_gridMin[1] + n; });
			std::sort(pending.begin(), pending.end(), [&](const auto& a, const auto& b) {
				const int64_t da = static_cast<int64_t>(a.first - centreX) * (a.first - centreX) + static_cast<int64_t>(a.second - centreY) * (a.second - centreY);
				const int64_t db = static_cast<int64_t>(b.first - centreX) * (b.first - centreX) + static_cast<int64_t>(b.second - centreY) * (b.second - centreY);
				return da > db;
			});
			pendingSorted = true;
		}

		auto* tes = RE::TES::GetSingleton();
		if (!tes || tes->interiorCell)
			return false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(RAY_BUDGET_MS));
		bool changed = false;
		while (!pending.empty() && std::chrono::steady_clock::now() < deadline) {
			const auto [cellX, cellY] = pending.back();
			pending.pop_back();
			if (cellX < a_gridMin[0] || cellX >= a_gridMin[0] + n || cellY < a_gridMin[1] || cellY >= a_gridMin[1] + n)
				continue;
			const std::size_t index = static_cast<std::size_t>(cellY & mask) * n + (cellX & mask);
			const float landHeight = land[index];
			// Unknown land, or water over it
			if (landHeight == NO_GROUND || heights[index] != landHeight)
				continue;
			const float worldX = (static_cast<float>(cellX) + 0.5f) * cellSize;
			const float worldY = (static_cast<float>(cellY) + 0.5f) * cellSize;
			auto* cell = tes->GetCell(RE::NiPoint3(worldX, worldY, landHeight));
			auto* bhkWorld = cell ? cell->GetbhkWorld() : nullptr;
			auto* world = bhkWorld ? bhkWorld->GetWorld1() : nullptr;
			if (!world)
				continue;
			float top;
			{
				const RE::BSReadLockGuard lock(bhkWorld->worldLock);
				top = CastDown(world, worldX, worldY, landHeight + RAY_ABOVE, landHeight - RAY_BELOW);
			}
			if (top > landHeight + 1.0f) {
				heights[index] = top;
				changed = true;
			}
		}
		return changed;
	}
}
