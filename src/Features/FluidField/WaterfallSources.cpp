#include "WaterfallSources.h"

#include "EngineGrid.h"
#include "WaterSurfaces.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace FluidFieldSim
{
	namespace
	{
		/** @brief The lowest part of a body (this fraction of its height) is where it lands. */
		constexpr float PLUNGE_BAND = 0.15f;
		/** @brief The plunge band is at least this tall, world units. */
		constexpr float MIN_PLUNGE_BAND = 32.0f;
		/** @brief The water surface may lie this far below a body's lowest point and still catch it. */
		constexpr float MAX_DROP_TO_WATER = 400.0f;
		/** @brief A body must start at least this far above the water it lands in. */
		constexpr float MIN_DROP = 64.0f;
		/** @brief A body whose top lies this close to a water's surface tips over from it: that water's brink. */
		constexpr float BRINK_MATCH = 128.0f;
		/** @brief Vertices kept per body to find where it meets the water. */
		constexpr std::size_t MAX_POINTS = 512;
		/** @brief Bodies landing closer than this are one fall. */
		constexpr float MERGE_DISTANCE = 400.0f;
		/** @brief Narrowest a fall's plunge counts as, world units. */
		constexpr float MIN_WIDTH = 96.0f;

		/** @brief D56: near zero; only marks a fall's emitter (spreading out from a point dented the water). */
		constexpr float RADIAL_SPEED = 0.001f;
		/** @brief Speed the water runs on in the fall's direction, world units per second. */
		constexpr float OUTFLOW_SPEED = 60.0f;
		/** @brief The current out of a landing at its edge (a plunge pool runs 2-3 m/s at the foot), world units per second. */
		constexpr float LANDING_SPEED = 180.0f;
		/** @brief The current into a brink at its edge, world units per second. */
		constexpr float BRINK_SPEED = 150.0f;

		/** @brief Foam made per second at the plunge. */
		constexpr float FOAM_RATE = 0.6f;

		std::string Lowercase(const char* a_text)
		{
			std::string result(a_text ? a_text : "");
			std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return result;
		}

		bool IsWaterfallBody(const char* a_model)
		{
			const std::string model = Lowercase(a_model);
			return model.find("effects\\") != std::string::npos && (model.find("waterfallbody") != std::string::npos || model.find("waterfallthin") != std::string::npos);
		}

		const char* ModelOf(const RE::TESBoundObject* a_base)
		{
			const auto* model = a_base ? a_base->As<RE::TESModel>() : nullptr;
			return model ? model->GetModel() : nullptr;
		}
	}

	void WaterfallSources::Clear()
	{
		bodies.clear();
		falls.clear();
		landings.clear();
		brinks.clear();
		updatesUntilScan = 0;
	}

	bool WaterfallSources::ReadBody(RE::TESObjectREFR& a_ref, Body& a_body)
	{
		auto* root = a_ref.Get3D();
		if (!root)
			return false;

		// World-space vertices, where the mesh keeps them on the CPU
		std::vector<RE::NiPoint3> points;
		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
			auto* triShape = a_geometry->AsTriShape();
			auto* renderer = a_geometry->GetGeometryRuntimeData().rendererData;
			if (!triShape || !renderer || !renderer->rawVertexData)
				return RE::BSVisit::BSVisitControl::kContinue;
			const auto& counts = triShape->GetTrishapeRuntimeData();
			const uint32_t stride = renderer->vertexDesc.GetSize();
			const uint32_t positionOffset = renderer->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_POSITION);
			if (stride < positionOffset + 12)
				return RE::BSVisit::BSVisitControl::kContinue;
			const auto& transform = a_geometry->world;
			for (uint32_t i = 0; i < counts.vertexCount; i++) {
				float local[3];
				std::memcpy(local, renderer->rawVertexData + static_cast<std::size_t>(i) * stride + positionOffset, sizeof(local));
				points.push_back(transform * RE::NiPoint3(local[0], local[1], local[2]));
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});

		// Without vertex data, the corners of the reference's bounds
		if (points.size() < 4) {
			points.clear();
			const auto boundMin = a_ref.GetBoundMin();
			const auto boundMax = a_ref.GetBoundMax();
			const auto& transform = root->world;
			for (int corner = 0; corner < 8; corner++) {
				const RE::NiPoint3 local((corner & 1) ? boundMax.x : boundMin.x, (corner & 2) ? boundMax.y : boundMin.y, (corner & 4) ? boundMax.z : boundMin.z);
				points.push_back(transform * local);
			}
		}

		float minZ = FLT_MAX;
		float maxZ = -FLT_MAX;
		for (const auto& p : points) {
			minZ = std::min(minZ, p.z);
			maxZ = std::max(maxZ, p.z);
		}
		const float band = std::max((maxZ - minZ) * PLUNGE_BAND, MIN_PLUNGE_BAND);

		// The plunge is the centre of the lowest band; the fall runs from the top band toward it
		RE::NiPoint3 low{}, high{};
		uint32_t lowCount = 0, highCount = 0;
		float lowMin[2] = { FLT_MAX, FLT_MAX }, lowMax[2] = { -FLT_MAX, -FLT_MAX };
		float highMin[2] = { FLT_MAX, FLT_MAX }, highMax[2] = { -FLT_MAX, -FLT_MAX };
		for (const auto& p : points) {
			if (p.z <= minZ + band) {
				low += p;
				lowCount++;
				lowMin[0] = std::min(lowMin[0], p.x);
				lowMin[1] = std::min(lowMin[1], p.y);
				lowMax[0] = std::max(lowMax[0], p.x);
				lowMax[1] = std::max(lowMax[1], p.y);
			}
			if (p.z >= maxZ - band) {
				high += p;
				highCount++;
				highMin[0] = std::min(highMin[0], p.x);
				highMin[1] = std::min(highMin[1], p.y);
				highMax[0] = std::max(highMax[0], p.x);
				highMax[1] = std::max(highMax[1], p.y);
			}
		}
		if (lowCount == 0 || highCount == 0)
			return false;
		low /= static_cast<float>(lowCount);
		high /= static_cast<float>(highCount);

		a_body.plunge[0] = low.x;
		a_body.plunge[1] = low.y;
		a_body.plunge[2] = low.z;
		a_body.lowest = minZ;
		a_body.highest = maxZ;
		a_body.top[0] = high.x;
		a_body.top[1] = high.y;
		a_body.topWidth = std::max(std::max(highMax[0] - highMin[0], highMax[1] - highMin[1]), MIN_WIDTH);
		const std::size_t step = (points.size() + MAX_POINTS - 1) / MAX_POINTS;
		for (std::size_t i = 0; i < points.size(); i += step)
			a_body.points.push_back(points[i]);
		a_body.width = std::max(std::max(lowMax[0] - lowMin[0], lowMax[1] - lowMin[1]), MIN_WIDTH);
		const float dx = low.x - high.x;
		const float dy = low.y - high.y;
		const float length = std::sqrt(dx * dx + dy * dy);
		a_body.direction[0] = length > 1.0f ? dx / length : 0.0f;
		a_body.direction[1] = length > 1.0f ? dy / length : 0.0f;
		return true;
	}

	void WaterfallSources::Update(const WaterSurfaces& a_water, float a_centreX, float a_centreY, float a_halfSize)
	{
		if (updatesUntilScan > 0) {
			updatesUntilScan--;
			return;
		}
		updatesUntilScan = SCAN_INTERVAL;

		auto* tes = RE::TES::GetSingleton();
		if (!tes)
			return;

		struct Found
		{
			Fall fall;
			float distance;
		};
		std::vector<Found> found[2];  // Landings, brinks
		const auto add = [&](int a_kind, float a_x, float a_y, float a_surface, float a_width, const float a_direction[2]) {
			Found f{};
			f.fall.plunge[0] = a_x;
			f.fall.plunge[1] = a_y;
			f.fall.plunge[2] = a_surface;
			f.fall.width = a_width;
			f.fall.direction[0] = a_direction[0];
			f.fall.direction[1] = a_direction[1];
			f.distance = std::hypot(a_x - a_centreX, a_y - a_centreY);
			found[a_kind].push_back(f);
		};
		const auto visitCell = [&](RE::TESObjectCELL* a_cell, bool a_requireAttached) {
			if (!a_cell || (a_requireAttached && !a_cell->IsAttached()))
				return;
			a_cell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
				if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted() || !a_ref->Get3D())
					return RE::BSContainer::ForEachResult::kContinue;
				if (!IsWaterfallBody(ModelOf(a_ref->GetBaseObject())))
					return RE::BSContainer::ForEachResult::kContinue;
				const auto position = a_ref->GetPosition();
				if (std::abs(position.x - a_centreX) > a_halfSize + 2048.0f || std::abs(position.y - a_centreY) > a_halfSize + 2048.0f)
					return RE::BSContainer::ForEachResult::kContinue;

				auto [it, added] = bodies.try_emplace(a_ref->GetFormID());
				if (added && !ReadBody(*a_ref, it->second)) {
					bodies.erase(it);
					return RE::BSContainer::ForEachResult::kContinue;
				}
				const Body& body = it->second;
				const bool landsInside = std::abs(body.plunge[0] - a_centreX) <= a_halfSize && std::abs(body.plunge[1] - a_centreY) <= a_halfSize;

				// A landing: the water's surface below its top and not far below its bottom; where the body crosses that
				// surface (a tall falls goes on far underwater), else its bottom
				float surface = 0.0f;
				if (landsInside && a_water.SampleHeight(body.plunge[0], body.plunge[1], surface) != WaterSurfaces::Source::None &&
					surface > body.lowest - MAX_DROP_TO_WATER && surface < body.highest - MIN_DROP) {
					const float band = std::max((body.highest - body.lowest) * PLUNGE_BAND, MIN_PLUNGE_BAND);
					float sum[2]{}, lo[2] = { FLT_MAX, FLT_MAX }, hi[2] = { -FLT_MAX, -FLT_MAX };
					uint32_t count = 0;
					for (const auto& p : body.points) {
						if (std::abs(p.z - surface) > band)
							continue;
						sum[0] += p.x;
						sum[1] += p.y;
						lo[0] = std::min(lo[0], p.x);
						lo[1] = std::min(lo[1], p.y);
						hi[0] = std::max(hi[0], p.x);
						hi[1] = std::max(hi[1], p.y);
						count++;
					}
					if (count > 0)
						add(0, sum[0] / count, sum[1] / count, surface, std::max(std::max(hi[0] - lo[0], hi[1] - lo[1]), MIN_WIDTH), body.direction);
					else
						add(0, body.plunge[0], body.plunge[1], surface, body.width, body.direction);
				}

				// A brink: its top at a water's surface, the water it drains. Looked for back upstream too: the top often sits
				// over the water below (another cell's level)
				if (std::abs(body.top[0] - a_centreX) <= a_halfSize && std::abs(body.top[1] - a_centreY) <= a_halfSize) {
					for (const float back : { 0.0f, 0.5f, 1.0f, 2.0f }) {
						const float x = body.top[0] - body.direction[0] * back * body.topWidth;
						const float y = body.top[1] - body.direction[1] * back * body.topWidth;
						float brinkSurface = 0.0f;
						if (a_water.SampleHeight(x, y, brinkSurface) != WaterSurfaces::Source::None && std::abs(brinkSurface - body.highest) <= BRINK_MATCH &&
							brinkSurface > body.lowest + MIN_DROP) {
							add(1, body.top[0], body.top[1], brinkSurface, body.topWidth, body.direction);
							break;
						}
					}
				}
				return RE::BSContainer::ForEachResult::kContinue;
			});
		};

		if (tes->interiorCell) {
			visitCell(tes->interiorCell, true);
		} else if (auto* grid = tes->gridCells) {
			for (uint32_t gx = 0; gx < grid->length; gx++) {
				for (uint32_t gy = 0; gy < grid->length; gy++)
					visitCell(GetGridCell(grid, gx, gy), true);
			}
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (auto* worldspace = player ? player->GetWorldspace() : nullptr)
				visitCell(worldspace->persistentCell, false);
		}

		// Bodies close together on the same water (a wide fall is several meshes) are one
		const auto merge = [](std::vector<Found>& a_found, std::vector<Fall>& a_out, std::size_t a_most) {
			std::sort(a_found.begin(), a_found.end(), [](const Found& a, const Found& b) { return a.distance < b.distance; });
			a_out.clear();
			std::vector<float> weights;
			for (const auto& f : a_found) {
				bool merged = false;
				for (std::size_t i = 0; i < a_out.size(); i++) {
					auto& fall = a_out[i];
					const float distance = std::hypot(f.fall.plunge[0] - fall.plunge[0], f.fall.plunge[1] - fall.plunge[1]);
					if (distance > MERGE_DISTANCE || std::abs(f.fall.plunge[2] - fall.plunge[2]) > MIN_DROP)
						continue;
					// Weighted by width; the merged fall spans both
					const float w = weights[i];
					const float lw = f.fall.width;
					for (int k = 0; k < 2; k++) {
						fall.plunge[k] = (fall.plunge[k] * w + f.fall.plunge[k] * lw) / (w + lw);
						fall.direction[k] = fall.direction[k] * w + f.fall.direction[k] * lw;
					}
					const float length = std::hypot(fall.direction[0], fall.direction[1]);
					if (length > 1e-3f) {
						fall.direction[0] /= length;
						fall.direction[1] /= length;
					}
					fall.width = std::max(fall.width, distance + 0.5f * (fall.width + lw));
					weights[i] = w + lw;
					merged = true;
					break;
				}
				if (!merged && a_out.size() < a_most) {
					a_out.push_back(f.fall);
					weights.push_back(f.fall.width);
				}
			}
		};
		const std::size_t before[2] = { landings.size(), brinks.size() };
		merge(found[0], landings, MAX_CURRENTS);
		merge(found[1], brinks, MAX_CURRENTS);
		if (landings.size() != before[0] || brinks.size() != before[1]) {
			logger::info("[Fluid Field] Falls currents: {} landings, {} brinks", landings.size(), brinks.size());
			for (int kind = 0; kind < 2; kind++) {
				for (const auto& f : kind == 0 ? landings : brinks)
					logger::info("[Fluid Field]   {} at ({:.0f}, {:.0f}), water {:.0f}, {:.0f} wide", kind == 0 ? "landing" : "brink", f.plunge[0], f.plunge[1], f.plunge[2], f.width);
			}
		}
		falls.assign(landings.begin(), landings.begin() + std::min(landings.size(), MAX_FALLS));

		// Forget bodies that unloaded
		for (auto it = bodies.begin(); it != bodies.end();) {
			auto* form = RE::TESForm::LookupByID(it->first);
			auto* ref = form ? form->AsReference() : nullptr;
			if (!ref || !ref->Get3D())
				it = bodies.erase(it);
			else
				++it;
		}
	}

	void WaterfallSources::AppendCurrents(std::vector<WaterFallCurrent>& a_currents, float a_strength) const
	{
		if (a_strength <= 0.0f)
			return;
		for (int kind = 0; kind < 2; kind++) {
			for (const auto& f : kind == 0 ? landings : brinks)
				a_currents.push_back({ .position = { f.plunge[0], f.plunge[1] },
					.direction = { f.direction[0], f.direction[1] },
					.halfWidth = 0.5f * f.width,
					.speed = (kind == 0 ? LANDING_SPEED : BRINK_SPEED) * a_strength,
					.level = f.plunge[2],
					.kind = kind == 0 ? 1.0f : -1.0f });
		}
	}

	void WaterfallSources::AppendEmitters(std::vector<WaterEmitter>& a_emitters, const int32_t a_gridMin[2], float a_cellSize, uint32_t a_gridRes,
		float a_strength, uint32_t a_maxEmitters, bool a_foam) const
	{
		if (a_strength <= 0.0f || !(a_cellSize > 0.0f))
			return;

		for (std::size_t i = 0; i < falls.size() && a_emitters.size() < a_maxEmitters; i++) {
			const auto& fall = falls[i];
			const float texel[2] = { fall.plunge[0] / a_cellSize - static_cast<float>(a_gridMin[0]), fall.plunge[1] / a_cellSize - static_cast<float>(a_gridMin[1]) };
			if (texel[0] < 0.0f || texel[1] < 0.0f || texel[0] >= static_cast<float>(a_gridRes) || texel[1] >= static_cast<float>(a_gridRes))
				continue;
			// Current running downstream from the plunge
			a_emitters.push_back({ .position = { texel[0], texel[1] },
				.radius = std::max(0.5f * fall.width / a_cellSize, 1.5f),
				.depthRate = 0.0f,  // D55: the falls drive the water's flow and foam, never dent its surface
				.velocity = { fall.direction[0] * OUTFLOW_SPEED * a_strength, fall.direction[1] * OUTFLOW_SPEED * a_strength },
				.pad1 = { RADIAL_SPEED * a_strength, a_foam ? FOAM_RATE * a_strength : 0.0f } });
		}
	}
}
