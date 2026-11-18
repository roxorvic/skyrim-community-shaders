#include "InteriorSun.h"
#include "I18n/I18n.h"
#include "Profiler.h"
#include "State.h"
#include "Utils/DebugSection.h"
#include "Utils/VersionedRelocation.h"

#define I18N_KEY_PREFIX "feature.interior_sun."

#include <numbers>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	InteriorSun::Settings,
	ForceDoubleSidedRendering,
	InteriorShadowDistance,
	WindowLightOnly,
	DetectOpenings,
	HideFakeWindowGlows,
	SkyLitGlass,
	SkyLitGlassStrength,
	AutoSunlitInteriors,
	ShellBlocksSun,
	ShellOffset,
	CloseShadowGaps,
	ShadowGapTexels,
	FrameShadowsFromGlowMaps,
	GlowMapThreshold)

namespace
{
	// Appends a shape's triangles in world space from its CPU geometry copy. False when the shape keeps none in a readable layout.
	bool AppendWorldTriangles(RE::BSGeometry* geometry, std::vector<float3>& vertices, std::vector<float2>* uvs = nullptr, float2 uvScale = { 1, 1 }, float2 uvOffset = { 0, 0 })
	{
		const auto triShape = geometry->AsTriShape();
		const auto rendererData = geometry->GetGeometryRuntimeData().rendererData;
		if (!triShape || !rendererData || !rendererData->rawVertexData || !rendererData->rawIndexData)
			return false;

		// Positions must be full-precision floats at offset 0
		auto& desc = rendererData->vertexDesc;
		const uint32_t stride = desc.GetSize();
		if (stride < 16 || desc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_TEXCOORD0) != 16)
			return false;

		const auto& counts = triShape->GetTrishapeRuntimeData();
		const size_t firstVertex = vertices.size();
		for (uint32_t i = 0; i < counts.triangleCount * 3u; ++i) {
			const uint32_t index = rendererData->rawIndexData[i];
			if (index >= counts.vertexCount) {
				vertices.resize(firstVertex);
				if (uvs)
					uvs->resize(firstVertex);
				return false;
			}
			const auto position = reinterpret_cast<const float*>(rendererData->rawVertexData + static_cast<size_t>(index) * stride);
			const auto world = geometry->world * RE::NiPoint3(position[0], position[1], position[2]);
			vertices.emplace_back(world.x, world.y, world.z);
			if (uvs) {
				const auto uv = reinterpret_cast<const uint16_t*>(rendererData->rawVertexData + static_cast<size_t>(index) * stride + 16);
				uvs->emplace_back(DirectX::PackedVector::XMConvertHalfToFloat(uv[0]) * uvScale.x + uvOffset.x, DirectX::PackedVector::XMConvertHalfToFloat(uv[1]) * uvScale.y + uvOffset.y);
			}
		}
		return true;
	}

	// Vanilla's painted window light: glow cards and dust beams, doubled by real sun (DWS's and Helios's lists)
	bool IsFakeWindowGlow(std::string_view model)
	{
		static constexpr std::string_view patterns[] = { "fxglowfillround", "fxglowflatrnd", "fxambbeam", "fxlightrays" };
		std::string lower(model);
		std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return std::ranges::any_of(patterns, [&](std::string_view pattern) { return lower.find(pattern) != std::string::npos; });
	}

	// Caves and dungeons have no windows to let the sun in by; Helios excludes the same location types
	bool IsUndergroundLocation(const RE::BGSLocation* location)
	{
		static constexpr std::string_view keywords[] = { "LocSetCave", "LocSetCaveIce", "LocSetDwarvenRuin", "LocSetNordicRuin", "LocTypeDungeon",
			"LocTypeDraugrCrypt", "LocTypeMine", "LocTypeDragonPriestLair", "LocTypeHagravenNest" };
		for (int depth = 0; location && depth < 8; ++depth, location = location->parentLoc) {
			if (std::ranges::any_of(keywords, [&](std::string_view keyword) { return location->HasKeywordString(keyword); }))
				return true;
		}
		return false;
	}

	// First hit of a ray against a triangle list, as a distance; FLT_MAX for none within reach (Moller-Trumbore)
	float CastRay(const float3& origin, const float3& direction, float reach, const std::vector<float3>& triangles, uint32_t first, uint32_t end)
	{
		float nearest = FLT_MAX;
		for (uint32_t i = first; i < end; i += 3) {
			const float3 edge1 = triangles[i + 1] - triangles[i];
			const float3 edge2 = triangles[i + 2] - triangles[i];
			const float3 p = direction.Cross(edge2);
			const float det = edge1.Dot(p);
			if (std::abs(det) < 1e-6f)
				continue;
			const float3 s = origin - triangles[i];
			const float u = s.Dot(p) / det;
			if (u < 0.0f || u > 1.0f)
				continue;
			const float3 q = s.Cross(edge1);
			const float v = direction.Dot(q) / det;
			if (v < 0.0f || u + v > 1.0f)
				continue;
			const float t = edge2.Dot(q) / det;
			if (t > 0.0f && t < reach && t < nearest)
				nearest = t;
		}
		return nearest;
	}

	bool RayHitsBox(const float3& origin, const float3& direction, float reach, const float3& boxMin, const float3& boxMax)
	{
		float enter = 0.0f, exit = reach;
		for (int axis = 0; axis < 3; ++axis) {
			const float o = (&origin.x)[axis], d = (&direction.x)[axis];
			const float lo = (&boxMin.x)[axis], hi = (&boxMax.x)[axis];
			if (std::abs(d) < 1e-8f) {
				if (o < lo || o > hi)
					return false;
				continue;
			}
			float t0 = (lo - o) / d, t1 = (hi - o) / d;
			if (t0 > t1)
				std::swap(t0, t1);
			enter = std::max(enter, t0);
			exit = std::min(exit, t1);
			if (enter > exit)
				return false;
		}
		return true;
	}
}

void InteriorSun::DrawSettings()
{
	ImGui::Checkbox(T(TKEY("force_double_sided"), "Force Double-Sided Rendering"), &settings.ForceDoubleSidedRendering);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("force_double_sided_tooltip"),
							  "Disables backface culling during sun shadowmap rendering in interiors. "
							  "Will prevent most light leaking through unmasked/unprepared interiors at a small performance cost. "));
	}
	if (ImGui::SliderFloat(T(TKEY("interior_shadow_distance"), "Interior Shadow Distance"), &settings.InteriorShadowDistance, 1000.0f, 8000.0f)) {
		*gInteriorShadowDistance = settings.InteriorShadowDistance;
		auto tes = RE::TES::GetSingleton();
		SetShadowDistance(tes && tes->interiorCell);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("interior_shadow_distance_tooltip"),
							  "Sets the distance shadows are rendered at in interiors. "
							  "Lower values provide higher quality shadows and improved performance but may cause distant interior spaces to light up incorrectly. "));
	}
	ImGui::Checkbox(T(TKEY("window_light_only"), "Sunlight Through Windows Only"), &settings.WindowLightOnly);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("window_light_only_tooltip"),
							  "Only lets the sun and moon into an interior through its window glass, which stops light leaking through walls, seams and roofs. "
							  "Has no effect in interiors whose windows are not marked as glass."));
	}
	ImGui::Checkbox(T(TKEY("frame_shadows_from_glow_maps"), "Window Frames Cast Shadows"), &settings.FrameShadowsFromGlowMaps);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("frame_shadows_from_glow_maps_tooltip"),
							  "Lets light through only the bright parts of each window's glow map, so frames and mullions cast shadows. "
							  "Works with window retextures through their own glow maps. Takes effect on entering an interior."));
	}
	if (settings.FrameShadowsFromGlowMaps)
		ImGui::SliderFloat(T(TKEY("glow_map_threshold"), "Window Frame Threshold"), &settings.GlowMapThreshold, 0.05f, 0.95f);
	ImGui::Checkbox(T(TKEY("detect_openings"), "Detect Openings"), &settings.DetectOpenings);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("detect_openings_tooltip"),
							  "Also lets light in through see-through shapes in the outer walls, such as lattices, that have nothing behind them. "
							  "Found automatically when entering an interior."));
	}
	ImGui::Checkbox(T(TKEY("hide_fake_window_glows"), "Hide Fake Window Glows"), &settings.HideFakeWindowGlows);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("hide_fake_window_glows_tooltip"),
							  "Hides the painted window glows and dust beams in interiors lit by the sun, where real light replaces them."));
	}
	ImGui::Checkbox(T(TKEY("sky_lit_glass"), "Sky-Lit Glass"), &settings.SkyLitGlass);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("sky_lit_glass_tooltip"),
							  "Makes glowing window glass follow the sky's colour and brightness in interiors lit by the sun. "
							  "Leave off when a weather mod already drives window glow."));
	}
	if (heliosLoaded && settings.SkyLitGlass)
		ImGui::TextDisabled("%s", T(TKEY("sky_lit_glass_helios"), "Off while Helios is loaded: Helios drives window glow itself."));
	if (settings.SkyLitGlass && !heliosLoaded)
		ImGui::SliderFloat(T(TKEY("sky_lit_glass_strength"), "Sky-Lit Glass Strength"), &settings.SkyLitGlassStrength, 0.0f, 4.0f);
	ImGui::Checkbox(T(TKEY("auto_sunlit_interiors"), "Automatic Sunlit Interiors"), &settings.AutoSunlitInteriors);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("auto_sunlit_interiors_tooltip"),
							  "Lets the sun into any interior with window glass or openings, even when no mod has set it up for sun. "
							  "Caves and dungeons are left out. Takes effect after entering the interior."));
	}
	ImGui::Checkbox(T(TKEY("shell_blocks_sun"), "Room Shell Blocks The Sun"), &settings.ShellBlocksSun);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("shell_blocks_sun_tooltip"),
							  "Draws the interior's own walls, floors and roofs into the sun's shadows from both sides, "
							  "so walls only built to be seen from inside still stop the sun. Needs Detect Openings."));
	}
	if (settings.ShellBlocksSun)
		ImGui::SliderFloat(T(TKEY("shell_offset"), "Room Shell Offset"), &settings.ShellOffset, 0.0f, 32.0f);
	ImGui::Checkbox(T(TKEY("close_shadow_gaps"), "Close Shadow Gaps"), &settings.CloseShadowGaps);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("close_shadow_gaps_tooltip"),
							  "In sunlit interiors without window holes, seals thin slivers of light at seams and wall edges in the sun's shadow map, "
							  "while larger openings stay open. Costs a little GPU time per shadow cascade."));
	}
	if (settings.CloseShadowGaps)
		ImGui::SliderInt(T(TKEY("shadow_gap_texels"), "Shadow Gap Width"), &settings.ShadowGapTexels, 1, 4);
}

void InteriorSun::DrawDebugSection()
{
	Util::DebugSection section;
	if (!section)
		return;

	ImGui::Text(T(TKEY("debug_window_glass"), "Window glass: %u shapes (%u with glow maps), %u triangles, %u unreadable shapes, %u hole draws"), windowGlassShapes, windowGlassShapesWithGlow, static_cast<uint32_t>(windowGlassVertices.size() / 3), windowGlassShapesSkipped, static_cast<uint32_t>(windowHoleDraws.size()));
	ImGui::Text(T(TKEY("debug_openings"), "Openings: %u of %u candidate shapes, %u triangles"), openingShapes, openingCandidates, static_cast<uint32_t>(windowOpeningVertices.size() / 3));
	ImGui::Text(T(TKEY("debug_scan_times"), "Scan times: glass %.2f ms (render thread), openings %.1f ms (worker)"), windowGlassScanMilliseconds, openingScanMilliseconds);
	ImGui::Text(T(TKEY("debug_window_holes"), "Shadow maps given window holes last frame: %u (%u calls in total with another target bound)"), windowHolesDrawnLastFrame, windowHolesTargetMismatches);
	for (size_t target = 0; target < windowHolesByTargetLastFrame.size(); ++target) {
		if (windowHolesByTargetLastFrame[target])
			ImGui::BulletText(T(TKEY("debug_window_holes_target"), "Depth target %u: %u"), static_cast<uint32_t>(target), windowHolesByTargetLastFrame[target]);
	}
	ImGui::Text(T(TKEY("debug_fake_glows"), "Fake window glows hidden: %u"), static_cast<uint32_t>(hiddenFakeGlows.size()));
	ImGui::Text(T(TKEY("debug_glass_emissives"), "Glowing glass shapes: %u"), static_cast<uint32_t>(glassEmissives.size()));
	ImGui::Text(T(TKEY("debug_auto_sunlit"), "Interiors given the sun automatically: %u"), static_cast<uint32_t>(autoSunlitCells.size()));
	ImGui::Text(T(TKEY("debug_shadow_gaps"), "Sun cascades with gaps closed last frame: %u"), shadowGapsClosedLastFrame);
	ImGui::Checkbox(T(TKEY("debug_show_holes"), "Show Holes On Screen (green glass, yellow openings, red rejected)"), &debugShowHoles);

	// The sun as the shadow maps use it, in this cell's own axes (indoors these need not match the building outside)
	const auto shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	const auto sunLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunShadowDirLight : nullptr;
	float3 toSun(0, 0, 0);
	if (sunLight) {
		const auto& v = sunLight->GetShadowDirectionalLightRuntimeData().sunVector;
		toSun = -float3(v.x, v.y, v.z);
		if (toSun.LengthSquared() > 1e-6f)
			toSun.Normalize();
		const float azimuth = std::atan2(toSun.x, toSun.y) * 180.0f / std::numbers::pi_v<float>;
		ImGui::Text(T(TKEY("debug_sun"), "Sun: azimuth %.0f deg (0 = cell +Y, clockwise), elevation %.0f deg"), azimuth < 0 ? azimuth + 360 : azimuth, std::asin(std::clamp(toSun.z, -1.0f, 1.0f)) * 180.0f / std::numbers::pi_v<float>);
	}

	// What the camera points at: the nearest scanned shape along its view
	if (const auto camera = RE::Main::WorldRootCamera()) {
		const auto& rotate = camera->world.rotate;
		const float3 origin(camera->world.translate.x, camera->world.translate.y, camera->world.translate.z);
		const float3 forward(rotate.entry[0][0], rotate.entry[1][0], rotate.entry[2][0]);
		float best = FLT_MAX;
		std::string what;
		for (const auto& glass : windowGlassShapeInfo) {
			const float t = CastRay(origin, forward, 100000.0f, windowGlassVertices, glass.first, glass.first + glass.count);
			if (t < best) {
				best = t;
				what = std::format("GLASS | ref {:08X} | {} | {}", glass.ref, glass.model, glass.shape);  // i18n-ignore: debug identifiers
			}
		}
		if (lastOpeningScan) {
			for (const auto& candidate : lastOpeningScan->candidates) {
				const float t = CastRay(origin, forward, 100000.0f, lastOpeningScan->candidateTriangles, candidate.firstTriangle, candidate.firstTriangle + candidate.triangleCount);
				if (t < best) {
					best = t;
					what = std::format("{} | ref {:08X} | {} | {} | {}/{} samples open | room side {:.0f} | sun in front: {}", candidate.opening ? "OPENING" : "REJECTED", candidate.ref, candidate.model, candidate.shape,  // i18n-ignore: debug identifiers
						candidate.openSamples, candidate.samples, candidate.nearSide, candidate.outward.Dot(toSun) > 0.0f ? "yes" : "no");
				}
			}
		}
		ImGui::TextWrapped(T(TKEY("debug_aimed"), "Aimed at (%.0f units): %s"), best == FLT_MAX ? 0.0f : best, best == FLT_MAX ? T(TKEY("debug_aimed_none"), "no scanned shape") : what.c_str());
	}

	if (lastOpeningScan) {
		uint32_t facing = 0, total = 0;
		for (const auto& candidate : lastOpeningScan->candidates) {
			if (candidate.opening) {
				++total;
				facing += candidate.outward.Dot(toSun) > 0.0f;
			}
		}
		ImGui::Text(T(TKEY("debug_openings_facing_sun"), "Openings with the sun in front of them now: %u of %u"), facing, total);
	}

	if (Util::Button(T(TKEY("debug_trace_sun"), "Trace Sun From Crosshair")))
		TraceSunFromCrosshair();
	ImGui::SameLine();
	if (Util::Button(T(TKEY("debug_trace_clear"), "Clear Trace")))
		traceValid = false;
	if (traceValid) {
		ImGui::TextWrapped(T(TKEY("debug_trace_from"), "From: %s"), traceStartWhat.c_str());
		if (traceHits.empty())
			ImGui::TextUnformatted(T(TKEY("debug_trace_open"), "Nothing between this point and the sun."));
		static constexpr const char* kinds[] = { "GLASS", "OPENING", "REJECTED", "SOLID" };  // i18n-ignore: debug labels
		for (const auto& hit : traceHits)
			ImGui::BulletText("%7.0f  %s  %s", hit.distance, kinds[hit.kind], hit.what.c_str());  // i18n-ignore: debug data
	}
	ImGui::Text(T(TKEY("debug_sun_caster_passes"), "Sun caster draws last frame: %u (kept one-sided: %u with the glass flag, %u skinned)"), sunCasterPassesLast[0], sunCasterPassesLast[1], sunCasterPassesLast[2]);
	ImGui::Text(T(TKEY("debug_shell"), "Room shell: %u solid shapes (%u without the cast-shadows flag, %u with the glass flag), %u triangles"), shellShapes, shellShapesNoCastFlag, shellShapesGlassFlag, shellCasterVertexCount / 3);
	ImGui::Checkbox(T(TKEY("debug_all_casters_two_sided"), "Every Sun Caster Double-Sided (ignore the glass and skinned conditions)"), &debugAllCastersTwoSided);
	ImGui::Checkbox(T(TKEY("debug_all_rooms_cast"), "All Rooms Cast Sun Shadows (ignore the sun-direction room filter)"), &debugAllRoomsCast);
	ImGui::Text(T(TKEY("debug_rooms_added"), "Rooms and portals added to the sun's casters: %u of %u"), roomsAddedLastFrame, roomsInCell);
	ImGui::Checkbox(T(TKEY("debug_show_hole_beams"), "Show Hole Beams (sun through every hole, ignoring what blocks it)"), &debugShowHoleBeams);
	if (Util::Button(T(TKEY("debug_rescan"), "Rescan Window Glass")))
		windowGlassCellFrames = 0;
}

void InteriorSun::LoadSettings(json& o_json)
{
	settings = o_json;
	if (gInteriorShadowDistance)
		*gInteriorShadowDistance = settings.InteriorShadowDistance;
}

void InteriorSun::SaveSettings(json& o_json)
{
	o_json = settings;
}

void InteriorSun::RestoreDefaultSettings()
{
	settings = {};
	if (gInteriorShadowDistance)
		*gInteriorShadowDistance = settings.InteriorShadowDistance;
}

void InteriorSun::PostPostLoad()
{
	stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));

	// Hooks and patch to enable directional lighting for interiors
	stl::write_thunk_call<GetWorldSpace>(REL::RelocationID(35562, 36561).address() + REL::Relocate(0x399, 0x37D));
	stl::write_thunk_call<GetWorldSpace>(REL::RelocationID(35562, 36561).address() + REL::Relocate(0x3AE, 0x392));
	REL::safe_fill(REL::RelocationID(35562, 36561).address() + REL::Relocate(0x397, 0x37B), REL::NOP, 2);

	// Hook for overriding the rooms and portals passed to the directional light culling step to fix light leaking through unrendered geometry
	stl::detour_thunk<DirShadowLightCulling>(REL::RelocationID(101498, 108492));

	// Hooks and patches in AIProcess::CalculateLightValue to force interior cells with directional lights to perform raycast checks
	REL::safe_fill(REL::RelocationID(38900, 39946).address() + REL::Relocate(0x1E7, 0x1F1), REL::NOP, REL::Module::IsAE() ? 2 : 6);
	stl::write_thunk_call<GetWorldSpace>(REL::RelocationID(38900, 39946).address() + REL::Relocate(0x1ED, 0x1F3));
	REL::safe_fill(REL::RelocationID(38900, 39946).address() + REL::Relocate(0x2CA, 0x22B), REL::NOP, REL::Module::IsAE() ? 6 : 2);

	gShadowDistance = reinterpret_cast<float*>(REL::RelocationID(528314, 415263).address());
	gInteriorShadowDistance = reinterpret_cast<float*>(REL::RelocationID(513755, 391724).address());
	*gInteriorShadowDistance = settings.InteriorShadowDistance;

	// Patches BSShadowDirectionalLight::SetFrameCamera to read the correct shadow distance value in interior cells
	const std::uintptr_t address = REL::RelocationID(101499, 108496).address() + Util::VersionedRelocation::Select(0xD62, 0xE6C, 0xE9C);
	const std::int32_t displacement = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(gShadowDistance) - (address + 8));
	REL::safe_write(address + 4, &displacement, sizeof(displacement));

	rasterStateCullMode = &globals::game::shadowState->GetRuntimeData().rasterStateCullMode;

	// Hook to draw window holes into the sun shadow maps before their casters. VR keeps a camera per eye in its descriptors, which is not handled.
	if (!REL::Module::IsVR())
		stl::write_vfunc<0x25, BSShaderAccumulator_StartAccumulating>(RE::VTABLE_BSShaderAccumulator[0]);

	logger::info("[Interior Sun] Installed hooks");
}

void InteriorSun::EarlyPrepass()
{
	const auto cell = RE::TES::GetSingleton()->interiorCell;
	isInteriorWithSun = IsInteriorWithSun(cell);
	if (!isInteriorWithSun && !arraysCleared) {
		ClearArrays();
		currentCell = nullptr;
		currentPortalGraph = nullptr;
	}

	if (!settings.AutoSunlitInteriors && !autoSunlitCells.empty())
		RestoreAutoSunlitCells();

	// Automatic mode scans interiors without the sun too, to decide whether to give them it
	if (cell && (isInteriorWithSun || settings.AutoSunlitInteriors))
		UpdateWindowGlass(cell);
	else if (windowGlassCell)
		ReleaseWindowGlass();

	if (!settings.HideFakeWindowGlows || !isInteriorWithSun)
		RestoreFakeGlows();
	UpdateSkyLitGlass();

	// The shadow maps were drawn just before this callback, casters included
	if (debugShowHoleBeams && !holeTargetsThisFrame.empty())
		DrawHoleBeams();
	holeTargetsThisFrame.clear();

	// Changing these needs the cell scanned again (openings, glows) and the holes rebuilt
	if (settings.DetectOpenings != lastDetectOpenings || settings.FrameShadowsFromGlowMaps != lastFrameShadows || settings.HideFakeWindowGlows != lastHideFakeGlows) {
		lastDetectOpenings = settings.DetectOpenings;
		lastFrameShadows = settings.FrameShadowsFromGlowMaps;
		lastHideFakeGlows = settings.HideFakeWindowGlows;
		lastCandidateTriangles.reset();
		windowOpeningVertices.clear();
		windowOpeningUVs.clear();
		windowOpeningRuns.clear();
		windowGlassCellFrames = 0;
		forceHolesUpload = true;
	}

	// Holes drawn into the maps make gap closing needless
	shadowGapsClosedLastFrame = 0;
	if (isInteriorWithSun && settings.CloseShadowGaps && !windowHolesDrawn)
		CloseShadowGaps();

	windowHolesDrawnLastFrame = std::exchange(windowHolesDrawn, 0);
	sunCasterPassesLast[0] = sunCasterPasses.exchange(0);
	sunCasterPassesLast[1] = sunCasterPassesGlassFlag.exchange(0);
	sunCasterPassesLast[2] = sunCasterPassesSkinned.exchange(0);
	windowHolesByTargetLastFrame = std::exchange(windowHolesByTarget, {});
	renderThread = std::this_thread::get_id();
}

void InteriorSun::SetupResources()
{
	auto device = globals::d3d::device;

	windowHolesCB = new ConstantBuffer(ConstantBufferDesc<WindowHolesCB>(), "InteriorSun::WindowHolesCB");
	shadowGapsCB = new ConstantBuffer(ConstantBufferDesc<ShadowGapsCB>(), "InteriorSun::ShadowGapsCB");

	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, windowHolesSampler.put()));
	Util::SetResourceName(windowHolesSampler.get(), "InteriorSun::WindowHolesSampler");

	D3D11_RASTERIZER_DESC rasterDesc{};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	DX::ThrowIfFailed(device->CreateRasterizerState(&rasterDesc, windowHolesRasterState.put()));
	Util::SetResourceName(windowHolesRasterState.get(), "InteriorSun::WindowHolesRasterState");

	D3D11_DEPTH_STENCIL_DESC depthDesc{};
	depthDesc.DepthEnable = true;
	depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
	DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, windowHolesDepthState.put()));
	Util::SetResourceName(windowHolesDepthState.get(), "InteriorSun::WindowHolesDepthState");

	depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, shellDepthState.put()));
	Util::SetResourceName(shellDepthState.get(), "InteriorSun::ShellDepthState");
}

void InteriorSun::ClearShaderCache()
{
	windowHolesVS = nullptr;
	windowHolesPS = nullptr;
	shadowGapsMinCS[0] = nullptr;
	shadowGapsMinCS[1] = nullptr;
	shadowGapsVS = nullptr;
	shadowGapsPS = nullptr;
}

ID3D11PixelShader* InteriorSun::GetWindowHolesPS()
{
	if (!windowHolesPS)
		windowHolesPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\InteriorSun\\WindowHolesVS.hlsl", {}, "ps_5_0", "psmain")));
	return windowHolesPS.get();
}

ID3D11VertexShader* InteriorSun::GetWindowHolesVS()
{
	if (!windowHolesVS)
		windowHolesVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\InteriorSun\\WindowHolesVS.hlsl", {}, "vs_5_0")));
	return windowHolesVS.get();
}

void InteriorSun::UpdateWindowGlass(RE::TESObjectCELL* cell)
{
	if (cell != windowGlassCell) {
		ReleaseWindowGlass();
		windowGlassCell = cell;
	}

	++windowGlassCellFrames;
	if (std::ranges::find(WindowGlassScanFrames, windowGlassCellFrames) != std::end(WindowGlassScanFrames))
		CollectWindowGlass(cell);
	TakeOpeningScan();
}

void InteriorSun::CollectWindowGlass(RE::TESObjectCELL* cell)
{
	globals::profiler->BeginPass("InteriorSun::WindowScan");
	const auto start = std::chrono::steady_clock::now();

	std::vector<float3> vertices;
	std::vector<float2> uvs;
	std::vector<HoleDraw> runs;
	std::vector<GlassShape> glassInfo;
	windowGlassShapes = 0;
	windowGlassShapesWithGlow = 0;
	windowGlassShapesSkipped = 0;
	auto scan = settings.DetectOpenings ? std::make_shared<OpeningScan>() : nullptr;
	const bool hideGlows = settings.HideFakeWindowGlows && isInteriorWithSun;

	cell->ForEachReference([&](RE::TESObjectREFR* ref) {
		const auto base = ref->GetBaseObject();
		const auto root = base ? ref->Get3D() : nullptr;
		if (!root)
			return RE::BSContainer::ForEachResult::kContinue;

		if (hideGlows && !root->GetAppCulled()) {
			const auto model = base->As<RE::TESModel>();
			if (model && IsFakeWindowGlow(model->GetModel())) {
				root->SetAppCulled(true);
				hiddenFakeGlows.emplace_back(root);
			}
		}

		// Windows and walls are statics; skipping everything else keeps the scan short
		if (!base->Is(RE::FormType::Static))
			return RE::BSContainer::ForEachResult::kContinue;

		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) {
			const auto& data = geometry->GetGeometryRuntimeData();
			const auto property = data.shaderProperty.get();
			if (!property)
				return RE::BSVisit::BSVisitControl::kContinue;

			// The flag mesh authors put on window glass, which also keeps it out of forced double-sided rendering
			if (property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kAssumeShadowmask)) {
				// Its glow map marks the panes, white on glass and black on the frame, so the frame can stay shadowed
				const auto lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(property);
				const auto material = lighting ? static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material) : nullptr;
				RE::NiPointer<RE::NiSourceTexture> glow;
				if (material && material->GetFeature() == RE::BSShaderMaterial::Feature::kGlowMap)
					glow = static_cast<const RE::BSLightingShaderMaterialGlowmap*>(material)->glowTexture;
				const float2 uvScale = material ? float2(material->texCoordScale[0].x, material->texCoordScale[0].y) : float2(1, 1);
				const float2 uvOffset = material ? float2(material->texCoordOffset[0].x, material->texCoordOffset[0].y) : float2(0, 0);
				const auto first = static_cast<uint32_t>(vertices.size());
				if (AppendWorldTriangles(geometry, vertices, &uvs, uvScale, uvOffset)) {
					++windowGlassShapes;
					if (glow && glow->rendererTexture && glow->rendererTexture->resourceView)
						++windowGlassShapesWithGlow;
					else
						glow = nullptr;
					runs.push_back({ first, static_cast<uint32_t>(vertices.size()) - first, glow });
					const auto model = base->As<RE::TESModel>();
					glassInfo.push_back({ ref->GetFormID(), model ? model->GetModel() : "", geometry->name.c_str(), first, static_cast<uint32_t>(vertices.size()) - first });
				} else
					++windowGlassShapesSkipped;
				if (lighting && lighting->emissiveColor && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kOwnEmit, RE::BSShaderProperty::EShaderPropertyFlag::kGlowMap) &&
					std::ranges::none_of(glassEmissives, [&](const auto& entry) { return entry.property.get() == lighting; }))
					glassEmissives.push_back({ RE::NiPointer<RE::BSLightingShaderProperty>(lighting), *lighting->emissiveColor, lighting->emissiveMult });
				return RE::BSVisit::BSVisitControl::kContinue;
			}
			if (!scan)
				return RE::BSVisit::BSVisitControl::kContinue;

			// See-through shapes that cast are candidates: as casters they still shadow their own solid parts once opened.
			// Everything else solid blocks the shell test's rays.
			const auto alpha = data.alphaProperty.get();
			if (alpha && alpha->GetAlphaTesting()) {
				if (!property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows))
					return RE::BSVisit::BSVisitControl::kContinue;
				// Only its see-through pixels can let light in, so it needs a readable diffuse texture
				const auto lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(property);
				const auto material = lighting ? static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material) : nullptr;
				const auto diffuse = material ? material->diffuseTexture : nullptr;
				if (!diffuse || !diffuse->rendererTexture || !diffuse->rendererTexture->resourceView)
					return RE::BSVisit::BSVisitControl::kContinue;
				const float2 uvScale(material->texCoordScale[0].x, material->texCoordScale[0].y);
				const float2 uvOffset(material->texCoordOffset[0].x, material->texCoordOffset[0].y);
				const auto first = static_cast<uint32_t>(scan->candidateTriangles.size());
				if (AppendWorldTriangles(geometry, scan->candidateTriangles, &scan->candidateUVs, uvScale, uvOffset)) {
					const auto model = base->As<RE::TESModel>();
					OpeningCandidate candidate{ model ? model->GetModel() : "", geometry->name.c_str(), ref->GetFormID(), first,
						static_cast<uint32_t>(scan->candidateTriangles.size()) - first };
					candidate.diffuse = diffuse;
					candidate.alphaThreshold = alpha->alphaThreshold / 255.0f;
					scan->candidates.push_back(std::move(candidate));
				}
			} else if (!alpha || !alpha->GetAlphaBlending()) {
				const auto first = static_cast<uint32_t>(scan->occluders.size());
				if (AppendWorldTriangles(geometry, scan->occluders)) {
					scan->occluderShapes.push_back(first);
					const auto model = base->As<RE::TESModel>();
					const bool casts = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kCastShadows);
					const bool glassFlag = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kAssumeShadowmask);
					const bool twoSided = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
					scan->occludersNoCastFlag += !casts;
					scan->occludersGlassFlag += glassFlag;
					scan->occluderNames.push_back(std::format("ref {:08X} | {} | {}{}{}{}", ref->GetFormID(), model ? model->GetModel() : "", geometry->name.c_str(),
						casts ? "" : " [no cast flag]", glassFlag ? " [glass flag]" : "", twoSided ? " [two-sided]" : ""));
				}
			}
			return RE::BSVisit::BSVisitControl::kContinue;
		});
		return RE::BSContainer::ForEachResult::kContinue;
	});

	windowGlassScanMilliseconds = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	globals::profiler->EndPass();

	// The shell test runs again only when the candidates changed since the last scan of this cell
	if (scan && !scan->candidates.empty() && (!lastCandidateTriangles || *lastCandidateTriangles != scan->candidateTriangles)) {
		scan->occluderShapes.push_back(static_cast<uint32_t>(scan->occluders.size()));
		scan->cell = cell;
		lastCandidateTriangles = std::make_shared<std::vector<float3>>(scan->candidateTriangles);
		openingScan = scan;
		std::thread([scan] {
			RunOpeningScan(*scan);
			scan->done = true;
		}).detach();
	}

	if (vertices != windowGlassVertices || std::exchange(forceHolesUpload, false)) {
		windowGlassVertices = std::move(vertices);
		windowGlassUVs = std::move(uvs);
		windowGlassRuns = std::move(runs);
		windowGlassShapeInfo = std::move(glassInfo);
		logger::info("[Interior Sun] Window glass in cell {:08X}: {} shapes ({} with glow maps), {} triangles, {} unreadable shapes, {:.2f} ms", cell->GetFormID(), windowGlassShapes, windowGlassShapesWithGlow, windowGlassVertices.size() / 3, windowGlassShapesSkipped, windowGlassScanMilliseconds);
		UploadWindowHoles(cell);
	}
	UpdateAutoSunlit(cell);
}

void InteriorSun::UploadWindowHoles(const RE::TESObjectCELL* cell)
{
	struct HoleVertex
	{
		float3 position;
		float2 uv;
	};
	std::vector<HoleVertex> holes;
	windowHoleDraws.clear();

	// Glass grouped by glow map, so each map is bound once; without frame shadows every pane opens whole
	auto runs = windowGlassRuns;
	for (auto& run : runs) {
		if (!settings.FrameShadowsFromGlowMaps)
			run.texture = nullptr;
		run.mode = run.texture ? kBrightGlow : kWhole;
	}
	auto openingRuns = settings.DetectOpenings ? windowOpeningRuns : std::vector<HoleDraw>{};
	const auto sameDraw = [](const HoleDraw& a, const HoleDraw& b) { return a.texture == b.texture && a.mode == b.mode && a.alphaThreshold == b.alphaThreshold; };
	const auto byTexture = [](const HoleDraw& run) { return run.texture.get(); };
	std::ranges::stable_sort(runs, {}, byTexture);
	std::ranges::stable_sort(openingRuns, {}, byTexture);
	const auto append = [&](const std::vector<HoleDraw>& list, const std::vector<float3>& positions, const std::vector<float2>& uvs) {
		for (const auto& run : list) {
			if (windowHoleDraws.empty() || !sameDraw(windowHoleDraws.back(), run))
				windowHoleDraws.push_back({ static_cast<uint32_t>(holes.size()), 0, run.texture, run.mode, run.alphaThreshold });
			for (uint32_t i = run.first; i < run.first + run.count; ++i)
				holes.push_back({ positions[i], uvs[i] });
			windowHoleDraws.back().count += run.count;
		}
	};
	append(runs, windowGlassVertices, windowGlassUVs);
	append(openingRuns, windowOpeningVertices, windowOpeningUVs);

	windowGlass.reset();
	windowGlassVertexCount = 0;
	if (holes.empty())
		return;

	try {
		const uint64_t count = holes.size();
		D3D11_SUBRESOURCE_DATA init{ holes.data(), 0, 0 };
		windowGlass = std::make_unique<Buffer>(StructuredBufferDesc<HoleVertex>(count, false), &init, "InteriorSun::WindowGlass");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = static_cast<UINT>(count);
		windowGlass->CreateSRV(srvDesc);
		windowGlassVertexCount = static_cast<uint32_t>(count);
	} catch (...) {
		logger::error("[Interior Sun] Failed to create the window holes buffer for cell {:08X}", cell->GetFormID());
		windowGlass.reset();
		windowHoleDraws.clear();
	}
}

void InteriorSun::TakeOpeningScan()
{
	if (!openingScan || !openingScan->done)
		return;
	const auto scan = std::exchange(openingScan, nullptr);
	if (scan->cell != windowGlassCell)
		return;
	lastOpeningScan = scan;

	windowOpeningVertices = std::move(scan->openings);
	windowOpeningUVs = std::move(scan->openingUVs);
	windowOpeningRuns = std::move(scan->openingRuns);
	openingCandidates = static_cast<uint32_t>(scan->candidates.size());
	openingShapes = static_cast<uint32_t>(std::ranges::count_if(scan->candidates, [](const auto& candidate) { return candidate.opening; }));
	openingScanMilliseconds = scan->milliseconds;

	logger::info("[Interior Sun] Openings in cell {:08X}: {} of {} candidate shapes, {} triangles, {:.1f} ms", scan->cell->GetFormID(), openingShapes, openingCandidates, windowOpeningVertices.size() / 3, openingScanMilliseconds);
	for (const auto& candidate : scan->candidates) {
		logger::info("[Interior Sun]   {} | ref {:08X} | {} | {} | {}/{} samples open | room side {:.0f}", candidate.opening ? "OPENING" : "no     ", candidate.ref, candidate.model, candidate.shape,
			candidate.openSamples, candidate.samples, candidate.nearSide);
	}

	UploadWindowHoles(scan->cell);
	UpdateAutoSunlit(scan->cell);

	// The room's shell for the sun maps, from the same triangles the shell test used
	shellShapes = static_cast<uint32_t>(scan->occluderNames.size());
	shellShapesNoCastFlag = scan->occludersNoCastFlag;
	shellShapesGlassFlag = scan->occludersGlassFlag;
	logger::info("[Interior Sun] Room shell in cell {:08X}: {} solid shapes ({} without the cast-shadows flag, {} with the glass flag), {} triangles",
		scan->cell->GetFormID(), shellShapes, shellShapesNoCastFlag, shellShapesGlassFlag, scan->occluders.size() / 3);
	shellCasters.reset();
	shellCasterVertexCount = 0;
	if (!scan->occluders.empty()) {
		struct ShellVertex
		{
			float3 position;
			float2 uv;
		};
		std::vector<ShellVertex> vertices;
		vertices.reserve(scan->occluders.size());
		for (const auto& position : scan->occluders)
			vertices.push_back({ position, float2(0, 0) });
		try {
			D3D11_SUBRESOURCE_DATA init{ vertices.data(), 0, 0 };
			shellCasters = std::make_unique<Buffer>(StructuredBufferDesc<ShellVertex>(vertices.size(), false), &init, "InteriorSun::ShellCasters");
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.NumElements = static_cast<UINT>(vertices.size());
			shellCasters->CreateSRV(srvDesc);
			shellCasterVertexCount = static_cast<uint32_t>(vertices.size());
		} catch (...) {
			logger::error("[Interior Sun] Failed to create the room shell buffer");
			shellCasters.reset();
		}
	}
}

void InteriorSun::RunOpeningScan(OpeningScan& scan)
{
	const auto start = std::chrono::steady_clock::now();
	// Far enough to cross the largest halls; beyond it, a ray has left the cell
	constexpr float Reach = 4096.0f;
	constexpr uint32_t SamplesPerShape = 4;

	struct Box
	{
		float3 min, max;
		uint32_t first, end;
	};
	std::vector<Box> boxes;
	for (size_t shape = 0; shape + 1 < scan.occluderShapes.size(); ++shape) {
		Box box{ float3(FLT_MAX), float3(-FLT_MAX), scan.occluderShapes[shape], scan.occluderShapes[shape + 1] };
		for (uint32_t i = box.first; i < box.end; ++i) {
			box.min = float3::Min(box.min, scan.occluders[i]);
			box.max = float3::Max(box.max, scan.occluders[i]);
		}
		if (box.first < box.end)
			boxes.push_back(box);
	}

	const auto cast = [&](const float3& origin, const float3& direction) {
		float nearest = FLT_MAX;
		for (const auto& box : boxes) {
			if (RayHitsBox(origin, direction, std::min(nearest, Reach), box.min, box.max))
				nearest = std::min(nearest, CastRay(origin, direction, Reach, scan.occluders, box.first, box.end));
		}
		return nearest;
	};

	for (auto& candidate : scan.candidates) {
		// Samples on the shape's largest triangles, each looking both ways along its facing
		std::vector<std::pair<float, uint32_t>> areas;
		for (uint32_t t = 0; t < candidate.triangleCount; t += 3) {
			const uint32_t i = candidate.firstTriangle + t;
			const auto& tri = scan.candidateTriangles;
			areas.emplace_back((tri[i + 1] - tri[i]).Cross(tri[i + 2] - tri[i]).Length(), i);
		}
		std::ranges::sort(areas, std::greater{});
		float nearSum = 0.0f;
		for (size_t s = 0; s < std::min<size_t>(SamplesPerShape, areas.size()); ++s) {
			const auto& tri = scan.candidateTriangles;
			const uint32_t i = areas[s].second;
			if (areas[s].first < 1e-3f)
				break;
			float3 normal = (tri[i + 1] - tri[i]).Cross(tri[i + 2] - tri[i]);
			normal.Normalize();
			const float3 centre = (tri[i] + tri[i + 1] + tri[i + 2]) / 3.0f;
			// Start a little off the shape, so a frame flush with it is not hit at zero distance
			const float front = cast(centre + normal * 2.0f, normal);
			const float back = cast(centre - normal * 2.0f, -normal);
			++candidate.samples;
			if ((front == FLT_MAX) != (back == FLT_MAX)) {
				++candidate.openSamples;
				nearSum += std::min(front, back);
				candidate.outward += front == FLT_MAX ? normal : -normal;
			}
		}
		candidate.nearSide = candidate.openSamples ? nearSum / candidate.openSamples : 0.0f;
		if (candidate.outward.LengthSquared() > 1e-6f)
			candidate.outward.Normalize();
		// Sun never comes up through a floor: rugs and floor pieces have only the void below them
		candidate.opening = candidate.samples && candidate.openSamples * 2 > candidate.samples && candidate.outward.z > -0.7f;
		if (candidate.opening) {
			const auto first = static_cast<uint32_t>(scan.openings.size());
			const auto begin = scan.candidateTriangles.begin() + candidate.firstTriangle;
			const auto uvBegin = scan.candidateUVs.begin() + candidate.firstTriangle;
			scan.openings.insert(scan.openings.end(), begin, begin + candidate.triangleCount);
			scan.openingUVs.insert(scan.openingUVs.end(), uvBegin, uvBegin + candidate.triangleCount);
			scan.openingRuns.push_back({ first, candidate.triangleCount, candidate.diffuse, kSeeThrough, candidate.alphaThreshold });
		}
	}
	scan.milliseconds = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void InteriorSun::UpdateAutoSunlit(RE::TESObjectCELL* cell)
{
	if (!settings.AutoSunlitInteriors || !cell || IsInteriorWithSun(cell) || autoSunlitCells.contains(cell) || !windowGlassVertexCount)
		return;
	if (IsUndergroundLocation(cell->GetLocation()))
		return;

	autoSunlitCells.emplace(cell, cell->cellFlags.underlying());
	cell->cellFlags.set(RE::TESObjectCELL::Flag::kShowSky, RE::TESObjectCELL::Flag::kUseSkyLighting, static_cast<RE::TESObjectCELL::Flag>(CellFlagExt::kSunlightShadows));
	logger::info("[Interior Sun] Gave cell {:08X} the sun automatically ({} window and opening triangles)", cell->GetFormID(), windowGlassVertexCount / 3);
}

void InteriorSun::RestoreAutoSunlitCells()
{
	for (const auto& [cell, flags] : autoSunlitCells)
		cell->cellFlags = static_cast<RE::TESObjectCELL::Flag>(flags);
	autoSunlitCells.clear();
}

void InteriorSun::RestoreFakeGlows()
{
	for (const auto& glow : hiddenFakeGlows)
		glow->SetAppCulled(false);
	hiddenFakeGlows.clear();
}

void InteriorSun::RestoreGlassEmissives()
{
	for (const auto& entry : glassEmissives) {
		*entry.property->emissiveColor = entry.color;
		entry.property->emissiveMult = entry.mult;
	}
}

void InteriorSun::UpdateSkyLitGlass()
{
	if (!settings.SkyLitGlass || !isInteriorWithSun || heliosLoaded) {
		if (glassEmissivesLit) {
			RestoreGlassEmissives();
			glassEmissivesLit = false;
		}
		return;
	}

	const auto sky = RE::Sky::GetSingleton();
	if (!sky)
		return;
	// The sky above: bright by day, dark at night, grey in rain
	const auto& color = sky->skyColor[static_cast<uint32_t>(RE::TESWeather::ColorTypes::kSkyUpper)];
	for (const auto& entry : glassEmissives) {
		*entry.property->emissiveColor = color;
		entry.property->emissiveMult = settings.SkyLitGlassStrength;
	}
	glassEmissivesLit = true;
}

void InteriorSun::ReleaseWindowGlass()
{
	RestoreFakeGlows();
	RestoreGlassEmissives();
	glassEmissivesLit = false;
	glassEmissives.clear();
	openingScan.reset();
	lastOpeningScan.reset();
	shellCasters.reset();
	shellCasterVertexCount = 0;
	windowGlassShapeInfo.clear();
	lastCandidateTriangles.reset();
	windowOpeningVertices.clear();
	windowOpeningUVs.clear();
	windowOpeningRuns.clear();
	openingCandidates = 0;
	openingShapes = 0;
	windowGlass.reset();
	windowGlassVertices.clear();
	windowGlassUVs.clear();
	windowGlassRuns.clear();
	windowHoleDraws.clear();
	windowGlassShapesWithGlow = 0;
	windowGlassCell = nullptr;
	windowGlassCellFrames = 0;
	windowGlassVertexCount = 0;
	windowGlassShapes = 0;
	windowGlassShapesSkipped = 0;
}

void InteriorSun::CloseShadowGaps()
{
	const auto shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	const auto sunLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunShadowDirLight : nullptr;
	if (!sunLight)
		return;

	auto context = globals::d3d::context;
	const auto& depthStencils = globals::game::renderer->GetDepthStencilData().depthStencils;
	const auto& descriptors = sunLight->GetRuntimeData().shadowmapDescriptors;
	const int radius = std::clamp(settings.ShadowGapTexels, 1, 4);

	if (!shadowGapsVS)
		shadowGapsVS.attach(static_cast<ID3D11VertexShader*>(Util::CompileShader(L"Data\\Shaders\\InteriorSun\\ShadowGapsMaxPS.hlsl", {}, "vs_5_0", "vsmain")));
	if (!shadowGapsPS)
		shadowGapsPS.attach(static_cast<ID3D11PixelShader*>(Util::CompileShader(L"Data\\Shaders\\InteriorSun\\ShadowGapsMaxPS.hlsl", {}, "ps_5_0")));
	if (!shadowGapsVS || !shadowGapsPS)
		return;

	globals::profiler->BeginPass("InteriorSun::ShadowGaps");

	// Everything changed here is put back: the engine caches its D3D state
	winrt::com_ptr<ID3D11ComputeShader> oldCS;
	winrt::com_ptr<ID3D11Buffer> oldCSCB;
	winrt::com_ptr<ID3D11ShaderResourceView> oldCSSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> oldCSUAV;
	winrt::com_ptr<ID3D11VertexShader> oldVS;
	winrt::com_ptr<ID3D11PixelShader> oldPS;
	winrt::com_ptr<ID3D11Buffer> oldPSCB;
	winrt::com_ptr<ID3D11ShaderResourceView> oldPSSRV;
	winrt::com_ptr<ID3D11InputLayout> oldInputLayout;
	D3D11_PRIMITIVE_TOPOLOGY oldTopology;
	winrt::com_ptr<ID3D11RasterizerState> oldRasterState;
	winrt::com_ptr<ID3D11DepthStencilState> oldDepthState;
	UINT oldStencilRef;
	D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT oldViewportCount = static_cast<UINT>(std::size(oldViewports));
	winrt::com_ptr<ID3D11RenderTargetView> oldRTV;
	winrt::com_ptr<ID3D11DepthStencilView> oldDSV;
	context->CSGetShader(oldCS.put(), nullptr, nullptr);
	context->CSGetConstantBuffers(0, 1, oldCSCB.put());
	context->CSGetShaderResources(0, 1, oldCSSRV.put());
	context->CSGetUnorderedAccessViews(0, 1, oldCSUAV.put());
	context->VSGetShader(oldVS.put(), nullptr, nullptr);
	context->PSGetShader(oldPS.put(), nullptr, nullptr);
	context->PSGetConstantBuffers(0, 1, oldPSCB.put());
	context->PSGetShaderResources(0, 1, oldPSSRV.put());
	context->IAGetInputLayout(oldInputLayout.put());
	context->IAGetPrimitiveTopology(&oldTopology);
	context->RSGetState(oldRasterState.put());
	context->RSGetViewports(&oldViewportCount, oldViewports);
	context->OMGetDepthStencilState(oldDepthState.put(), &oldStencilRef);
	context->OMGetRenderTargets(1, oldRTV.put(), oldDSV.put());

	const auto count = std::min(sunLight->shadowMapCount, descriptors.size());
	for (uint32_t i = 0; i < count; ++i) {
		const auto target = static_cast<uint32_t>(descriptors[i].renderTarget);
		const auto slice = descriptors[i].shadowmapIndex;
		if (target >= RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL || slice >= std::size(depthStencils[target].views))
			continue;
		const auto& depth = depthStencils[target];
		if (!depth.texture || !depth.depthSRV || !depth.views[slice])
			continue;

		D3D11_TEXTURE2D_DESC textureDesc;
		depth.texture->GetDesc(&textureDesc);
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		depth.depthSRV->GetDesc(&srvDesc);
		const bool isArray = srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY;

		auto& minCS = shadowGapsMinCS[isArray];
		if (!minCS) {
			std::vector<std::pair<const char*, const char*>> defines;
			if (isArray)
				defines.emplace_back("SHADOW_ARRAY", "");
			minCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\InteriorSun\\ShadowGapsMinCS.hlsl", defines, "cs_5_0")));
			if (!minCS)
				continue;
		}

		if (!shadowGapsGrown || shadowGapsGrown->desc.Width != textureDesc.Width || shadowGapsGrown->desc.Height != textureDesc.Height) {
			D3D11_TEXTURE2D_DESC grownDesc{};
			grownDesc.Width = textureDesc.Width;
			grownDesc.Height = textureDesc.Height;
			grownDesc.MipLevels = 1;
			grownDesc.ArraySize = 1;
			grownDesc.Format = DXGI_FORMAT_R32_FLOAT;
			grownDesc.SampleDesc.Count = 1;
			grownDesc.Usage = D3D11_USAGE_DEFAULT;
			grownDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			shadowGapsGrown = std::make_unique<Texture2D>(grownDesc, "InteriorSun::ShadowGapsGrown");
			D3D11_SHADER_RESOURCE_VIEW_DESC grownSRV{ DXGI_FORMAT_R32_FLOAT, D3D11_SRV_DIMENSION_TEXTURE2D };
			grownSRV.Texture2D.MipLevels = 1;
			shadowGapsGrown->CreateSRV(grownSRV);
			D3D11_UNORDERED_ACCESS_VIEW_DESC grownUAV{ DXGI_FORMAT_R32_FLOAT, D3D11_UAV_DIMENSION_TEXTURE2D };
			shadowGapsGrown->CreateUAV(grownUAV);
		}

		ShadowGapsCB data{ { textureDesc.Width, textureDesc.Height }, slice, radius };
		shadowGapsCB->Update(data);
		auto cb = shadowGapsCB->CB();

		// Grow the shadows into the scratch texture; the depth must not be bound for writing meanwhile
		context->OMSetRenderTargets(0, nullptr, nullptr);
		ID3D11ShaderResourceView* srv = depth.depthSRV;
		ID3D11UnorderedAccessView* uav = shadowGapsGrown->uav.get();
		context->CSSetShader(minCS.get(), nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &cb);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->Dispatch((textureDesc.Width + 7) / 8, (textureDesc.Height + 7) / 8, 1);
		srv = nullptr;
		uav = nullptr;
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

		// Shrink them back while writing the cascade's depth
		D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(textureDesc.Width), static_cast<float>(textureDesc.Height), 0.0f, 1.0f };
		srv = shadowGapsGrown->srv.get();
		context->OMSetRenderTargets(0, nullptr, depth.views[slice]);
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(shadowGapsVS.get(), nullptr, 0);
		context->PSSetShader(shadowGapsPS.get(), nullptr, 0);
		context->PSSetConstantBuffers(0, 1, &cb);
		context->PSSetShaderResources(0, 1, &srv);
		context->RSSetState(windowHolesRasterState.get());
		context->RSSetViewports(1, &viewport);
		context->OMSetDepthStencilState(windowHolesDepthState.get(), 0);
		context->Draw(3, 0);
		srv = nullptr;
		context->PSSetShaderResources(0, 1, &srv);
		++shadowGapsClosedLastFrame;
	}

	auto restore = oldCSCB.get();
	auto restoreSRV = oldCSSRV.get();
	auto restoreUAV = oldCSUAV.get();
	context->CSSetShader(oldCS.get(), nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &restore);
	context->CSSetShaderResources(0, 1, &restoreSRV);
	context->CSSetUnorderedAccessViews(0, 1, &restoreUAV, nullptr);
	restore = oldPSCB.get();
	restoreSRV = oldPSSRV.get();
	context->VSSetShader(oldVS.get(), nullptr, 0);
	context->PSSetShader(oldPS.get(), nullptr, 0);
	context->PSSetConstantBuffers(0, 1, &restore);
	context->PSSetShaderResources(0, 1, &restoreSRV);
	context->IASetInputLayout(oldInputLayout.get());
	context->IASetPrimitiveTopology(oldTopology);
	context->RSSetState(oldRasterState.get());
	context->RSSetViewports(oldViewportCount, oldViewports);
	context->OMSetDepthStencilState(oldDepthState.get(), oldStencilRef);
	auto restoreRTV = oldRTV.get();
	context->OMSetRenderTargets(1, &restoreRTV, oldDSV.get());

	globals::profiler->EndPass();
}

void InteriorSun::DrawWindowHoles(const RE::NiCamera* camera)
{
	const auto shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	const auto sunLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunShadowDirLight : nullptr;
	if (!sunLight)
		return;

	// Only the sun's cascades: one of their descriptors holds the camera about to be rendered
	const auto& descriptors = sunLight->GetRuntimeData().shadowmapDescriptors;
	const auto descriptorsEnd = descriptors.begin() + std::min(sunLight->shadowMapCount, descriptors.size());
	const auto descriptor = std::find_if(descriptors.begin(), descriptorsEnd, [&](const auto& a_descriptor) { return a_descriptor.camera.get() == camera; });
	if (descriptor == descriptorsEnd)
		return;

	// The engine has just bound and cleared the cascade's slice. Anything else bound means this is not that moment, so leave it alone.
	auto context = globals::d3d::context;
	const auto& depthStencils = globals::game::renderer->GetDepthStencilData().depthStencils;
	const auto target = static_cast<uint32_t>(descriptor->renderTarget);
	const auto slice = descriptor->shadowmapIndex;
	winrt::com_ptr<ID3D11DepthStencilView> dsv;
	context->OMGetRenderTargets(0, nullptr, dsv.put());
	if (target >= RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL || slice >= std::size(depthStencils[target].views) || !dsv || dsv.get() != depthStencils[target].views[slice]) {
		++windowHolesTargetMismatches;
		return;
	}

	D3D11_TEXTURE2D_DESC textureDesc;
	depthStencils[target].texture->GetDesc(&textureDesc);

	// The engine set this camera up right before, so these are the matrices its casters will use
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	const auto eye = shadowState.cameraData.getEye();
	const auto posAdjust = shadowState.posAdjust.getEye();

	WindowHolesCB data{};
	data.ViewProj = eye.viewProjMat;
	data.PosAdjust = { posAdjust.x, posAdjust.y, posAdjust.z };
	data.GlowThreshold = settings.GlowMapThreshold;

	// Same viewport as the engine derives from the camera's port (left, right, top, bottom)
	D3D11_VIEWPORT viewport{};
	viewport.TopLeftX = eye.viewPort[0] * textureDesc.Width;
	viewport.TopLeftY = (1.0f - eye.viewPort[2]) * textureDesc.Height;
	viewport.Width = (eye.viewPort[1] - eye.viewPort[0]) * textureDesc.Width;
	viewport.Height = (eye.viewPort[2] - eye.viewPort[3]) * textureDesc.Height;
	viewport.MaxDepth = 1.0f;

	globals::profiler->BeginPass("InteriorSun::WindowHoles");
	// Everything is shadowed from the sun except through the panes; the casters drawn next still shadow that light
	context->ClearDepthStencilView(dsv.get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
	DrawHolesInto(data, viewport);
	globals::profiler->EndPass();

	if (settings.ShellBlocksSun && shellCasters && shellCasterVertexCount) {
		globals::profiler->BeginPass("InteriorSun::ShellCasters");
		const auto& v = sunLight->GetShadowDirectionalLightRuntimeData().sunVector;
		float3 awayFromSun(v.x, v.y, v.z);
		awayFromSun.Normalize();
		WindowHolesCB shell = data;
		shell.Mode = kShellCaster;
		shell.ShellOffset = awayFromSun * settings.ShellOffset;
		windowHolesCB->Update(shell);

		winrt::com_ptr<ID3D11VertexShader> oldVS;
		winrt::com_ptr<ID3D11PixelShader> oldPS;
		winrt::com_ptr<ID3D11Buffer> oldCB;
		winrt::com_ptr<ID3D11ShaderResourceView> oldSRV;
		winrt::com_ptr<ID3D11InputLayout> oldInputLayout;
		D3D11_PRIMITIVE_TOPOLOGY oldTopology;
		winrt::com_ptr<ID3D11RasterizerState> oldRasterState;
		winrt::com_ptr<ID3D11DepthStencilState> oldDepthState;
		UINT oldStencilRef;
		D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		UINT oldViewportCount = static_cast<UINT>(std::size(oldViewports));
		context->VSGetShader(oldVS.put(), nullptr, nullptr);
		context->PSGetShader(oldPS.put(), nullptr, nullptr);
		context->VSGetConstantBuffers(0, 1, oldCB.put());
		context->VSGetShaderResources(0, 1, oldSRV.put());
		context->IAGetInputLayout(oldInputLayout.put());
		context->IAGetPrimitiveTopology(&oldTopology);
		context->RSGetState(oldRasterState.put());
		context->RSGetViewports(&oldViewportCount, oldViewports);
		context->OMGetDepthStencilState(oldDepthState.put(), &oldStencilRef);

		auto cb = windowHolesCB->CB();
		auto srv = shellCasters->srv.get();
		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(GetWindowHolesVS(), nullptr, 0);
		context->PSSetShader(nullptr, nullptr, 0);
		context->VSSetConstantBuffers(0, 1, &cb);
		context->VSSetShaderResources(0, 1, &srv);
		context->RSSetState(windowHolesRasterState.get());  // no culling: walls block from outside too
		context->RSSetViewports(1, &viewport);
		context->OMSetDepthStencilState(shellDepthState.get(), 0);
		context->Draw(shellCasterVertexCount, 0);

		cb = oldCB.get();
		srv = oldSRV.get();
		context->VSSetShader(oldVS.get(), nullptr, 0);
		context->PSSetShader(oldPS.get(), nullptr, 0);
		context->VSSetConstantBuffers(0, 1, &cb);
		context->VSSetShaderResources(0, 1, &srv);
		context->IASetInputLayout(oldInputLayout.get());
		context->IASetPrimitiveTopology(oldTopology);
		context->RSSetState(oldRasterState.get());
		context->RSSetViewports(oldViewportCount, oldViewports);
		context->OMSetDepthStencilState(oldDepthState.get(), oldStencilRef);
		globals::profiler->EndPass();
	}

	if (debugShowHoleBeams)
		holeTargetsThisFrame.push_back({ dsv.get(), data.ViewProj, data.PosAdjust, viewport });
	++windowHolesDrawn;
	++windowHolesByTarget[target];
}

void InteriorSun::DrawHolesInto(WindowHolesCB data, const D3D11_VIEWPORT& viewport)
{
	auto context = globals::d3d::context;
	const auto vs = GetWindowHolesVS();
	if (!vs || !windowGlass)
		return;

	// The engine caches its D3D state, so everything this draw changes is put back
	winrt::com_ptr<ID3D11InputLayout> oldInputLayout;
	D3D11_PRIMITIVE_TOPOLOGY oldTopology;
	winrt::com_ptr<ID3D11VertexShader> oldVS;
	winrt::com_ptr<ID3D11PixelShader> oldPS;
	winrt::com_ptr<ID3D11Buffer> oldCB;
	winrt::com_ptr<ID3D11ShaderResourceView> oldSRV;
	winrt::com_ptr<ID3D11Buffer> oldPSCB;
	winrt::com_ptr<ID3D11ShaderResourceView> oldPSSRV;
	winrt::com_ptr<ID3D11SamplerState> oldSampler;
	winrt::com_ptr<ID3D11RasterizerState> oldRasterState;
	winrt::com_ptr<ID3D11DepthStencilState> oldDepthState;
	UINT oldStencilRef;
	D3D11_VIEWPORT oldViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT oldViewportCount = static_cast<UINT>(std::size(oldViewports));
	context->IAGetInputLayout(oldInputLayout.put());
	context->IAGetPrimitiveTopology(&oldTopology);
	context->VSGetShader(oldVS.put(), nullptr, nullptr);
	context->PSGetShader(oldPS.put(), nullptr, nullptr);
	context->VSGetConstantBuffers(0, 1, oldCB.put());
	context->VSGetShaderResources(0, 1, oldSRV.put());
	context->PSGetConstantBuffers(0, 1, oldPSCB.put());
	context->PSGetShaderResources(0, 1, oldPSSRV.put());
	context->PSGetSamplers(0, 1, oldSampler.put());
	context->RSGetState(oldRasterState.put());
	context->RSGetViewports(&oldViewportCount, oldViewports);
	context->OMGetDepthStencilState(oldDepthState.put(), &oldStencilRef);

	auto cb = windowHolesCB->CB();
	auto srv = windowGlass->srv.get();
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vs, nullptr, 0);
	context->VSSetConstantBuffers(0, 1, &cb);
	context->VSSetShaderResources(0, 1, &srv);
	context->PSSetConstantBuffers(0, 1, &cb);
	auto sampler = windowHolesSampler.get();
	context->PSSetSamplers(0, 1, &sampler);
	context->RSSetState(windowHolesRasterState.get());
	context->RSSetViewports(1, &viewport);
	context->OMSetDepthStencilState(windowHolesDepthState.get(), 0);
	const auto ps = GetWindowHolesPS();
	for (const auto& draw : windowHoleDraws) {
		// With a texture, a pixel shader keeps the solid parts shadowed (frames, lattice bars); without one, depth only
		const bool textured = draw.mode != kWhole && draw.texture && ps;
		if (draw.mode == kSeeThrough && !textured)
			continue;  // an opening that can't be told see-through from solid stays closed
		data.Mode = textured ? draw.mode : kWhole;
		data.AlphaThreshold = draw.alphaThreshold;
		windowHolesCB->Update(data);
		ID3D11ShaderResourceView* texture = textured ? draw.texture->rendererTexture->resourceView : nullptr;
		context->PSSetShader(textured ? ps : nullptr, nullptr, 0);
		context->PSSetShaderResources(0, 1, &texture);
		context->Draw(draw.count, draw.first);
	}

	cb = oldCB.get();
	srv = oldSRV.get();
	context->IASetInputLayout(oldInputLayout.get());
	context->IASetPrimitiveTopology(oldTopology);
	context->VSSetShader(oldVS.get(), nullptr, 0);
	context->PSSetShader(oldPS.get(), nullptr, 0);
	context->VSSetConstantBuffers(0, 1, &cb);
	context->VSSetShaderResources(0, 1, &srv);
	cb = oldPSCB.get();
	srv = oldPSSRV.get();
	sampler = oldSampler.get();
	context->PSSetConstantBuffers(0, 1, &cb);
	context->PSSetShaderResources(0, 1, &srv);
	context->PSSetSamplers(0, 1, &sampler);
	context->RSSetState(oldRasterState.get());
	context->RSSetViewports(oldViewportCount, oldViewports);
	context->OMSetDepthStencilState(oldDepthState.get(), oldStencilRef);
}

void InteriorSun::DrawHoleBeams()
{
	auto context = globals::d3d::context;
	winrt::com_ptr<ID3D11RenderTargetView> oldRTV;
	winrt::com_ptr<ID3D11DepthStencilView> oldDSV;
	context->OMGetRenderTargets(1, oldRTV.put(), oldDSV.put());
	for (const auto& holeTarget : holeTargetsThisFrame) {
		WindowHolesCB data{};
		data.ViewProj = holeTarget.viewProj;
		data.PosAdjust = holeTarget.posAdjust;
		data.GlowThreshold = settings.GlowMapThreshold;
		context->OMSetRenderTargets(0, nullptr, holeTarget.dsv);
		DrawHolesInto(data, holeTarget.viewport);
	}
	auto restoreRTV = oldRTV.get();
	context->OMSetRenderTargets(1, &restoreRTV, oldDSV.get());
}

void InteriorSun::TraceSunFromCrosshair()
{
	traceValid = false;
	traceHits.clear();
	const auto camera = RE::Main::WorldRootCamera();
	const auto shadowSceneNode = globals::game::smState->shadowSceneNode[0];
	const auto sunLight = shadowSceneNode ? shadowSceneNode->GetRuntimeData().sunShadowDirLight : nullptr;
	if (!camera || !sunLight || !lastOpeningScan)
		return;
	const auto& scan = *lastOpeningScan;

	// Every scanned shape, nearest crossing per shape, along a ray (glass, candidates, solid occluders)
	const auto crossings = [&](const float3& origin, const float3& direction, float minDistance) {
		std::vector<TraceHit> hits;
		for (const auto& glass : windowGlassShapeInfo) {
			const float t = CastRay(origin, direction, 50000.0f, windowGlassVertices, glass.first, glass.first + glass.count);
			if (t > minDistance && t != FLT_MAX)
				hits.push_back({ t, 0, std::format("ref {:08X} | {} | {}", glass.ref, glass.model, glass.shape) });
		}
		for (const auto& candidate : scan.candidates) {
			const float t = CastRay(origin, direction, 50000.0f, scan.candidateTriangles, candidate.firstTriangle, candidate.firstTriangle + candidate.triangleCount);
			if (t > minDistance && t != FLT_MAX)
				hits.push_back({ t, candidate.opening ? 1 : 2, std::format("ref {:08X} | {} | {}", candidate.ref, candidate.model, candidate.shape) });
		}
		for (size_t shape = 0; shape + 1 < scan.occluderShapes.size(); ++shape) {
			const uint32_t first = scan.occluderShapes[shape], end = scan.occluderShapes[shape + 1];
			float3 boxMin(FLT_MAX), boxMax(-FLT_MAX);
			for (uint32_t i = first; i < end; ++i) {
				boxMin = float3::Min(boxMin, scan.occluders[i]);
				boxMax = float3::Max(boxMax, scan.occluders[i]);
			}
			if (!RayHitsBox(origin, direction, 50000.0f, boxMin, boxMax))
				continue;
			const float t = CastRay(origin, direction, 50000.0f, scan.occluders, first, end);
			if (t > minDistance && t != FLT_MAX)
				hits.push_back({ t, 3, shape < scan.occluderNames.size() ? scan.occluderNames[shape] : std::string() });
		}
		std::ranges::sort(hits, {}, &TraceHit::distance);
		return hits;
	};

	// The surface under the crosshair
	const auto& rotate = camera->world.rotate;
	const float3 eye(camera->world.translate.x, camera->world.translate.y, camera->world.translate.z);
	const float3 forward(rotate.entry[0][0], rotate.entry[1][0], rotate.entry[2][0]);
	const auto view = crossings(eye, forward, 1.0f);
	if (view.empty())
		return;
	traceStartWhat = view.front().what;
	// A little back toward the camera, so the surface itself is not counted as blocking its own light
	traceStart = eye + forward * (view.front().distance - 2.0f);

	const auto& v = sunLight->GetShadowDirectionalLightRuntimeData().sunVector;
	traceDirection = -float3(v.x, v.y, v.z);
	traceDirection.Normalize();
	traceHits = crossings(traceStart, traceDirection, 0.5f);
	traceValid = true;

	logger::info("[Interior Sun] Sun trace from {} (sun {:.2f} {:.2f} {:.2f}): {} crossings", traceStartWhat, traceDirection.x, traceDirection.y, traceDirection.z, traceHits.size());
	static constexpr const char* kinds[] = { "GLASS", "OPENING", "REJECTED", "SOLID" };
	for (const auto& hit : traceHits)
		logger::info("[Interior Sun]   {:7.0f} | {} | {}", hit.distance, kinds[hit.kind], hit.what);
}

void InteriorSun::DrawOverlay()
{
	const auto camera = RE::Main::WorldRootCamera();
	if (!camera || !windowGlassCell)
		return;

	auto drawList = ImGui::GetBackgroundDrawList();
	const auto size = ImGui::GetIO().DisplaySize;
	const auto& port = camera->GetRuntimeData2().port;
	const auto project = [&](const float3& point, ImVec2& out) {
		float x, y, z;
		if (!RE::NiCamera::WorldPtToScreenPt3(camera->GetRuntimeData().worldToCam, port, RE::NiPoint3(point.x, point.y, point.z), x, y, z, 1e-5f) || z < 0.0f)
			return false;
		out = ImVec2(x * size.x, (1.0f - y) * size.y);
		return true;
	};
	const auto draw = [&](const std::vector<float3>& triangles, uint32_t first, uint32_t count, ImU32 color) {
		for (uint32_t i = first; i + 2 < first + count; i += 3) {
			ImVec2 a, b, c;
			if (project(triangles[i], a) && project(triangles[i + 1], b) && project(triangles[i + 2], c))
				drawList->AddTriangleFilled(a, b, c, color);
		}
	};

	// The traced sun ray: from the aimed point toward the sun, a marker at each shape it crosses
	if (traceValid) {
		const float length = traceHits.empty() ? 3000.0f : traceHits.back().distance + 500.0f;
		ImVec2 a, b;
		if (project(traceStart, a) && project(traceStart + traceDirection * length, b))
			drawList->AddLine(a, b, ImGui::GetColorU32(ImVec4(0.3f, 0.7f, 1.0f, 1.0f)), 3.0f);
		if (project(traceStart, a))
			drawList->AddCircleFilled(a, 8.0f, ImGui::GetColorU32(ImVec4(0.3f, 0.7f, 1.0f, 1.0f)));
		static const ImVec4 colors[] = { ImVec4(0.1f, 1.0f, 0.2f, 1.0f), ImVec4(1.0f, 0.9f, 0.1f, 1.0f), ImVec4(1.0f, 0.15f, 0.1f, 1.0f), ImVec4(1.0f, 1.0f, 1.0f, 1.0f) };
		for (const auto& hit : traceHits) {
			if (project(traceStart + traceDirection * hit.distance, a))
				drawList->AddCircleFilled(a, 7.0f, ImGui::GetColorU32(colors[hit.kind]));
		}
	}
	if (!debugShowHoles)
		return;

	// Drawn over everything, walls included: what matters is where each hole is
	draw(windowGlassVertices, 0, static_cast<uint32_t>(windowGlassVertices.size()), ImGui::GetColorU32(ImVec4(0.1f, 1.0f, 0.2f, 0.35f)));
	if (lastOpeningScan) {
		for (const auto& candidate : lastOpeningScan->candidates)
			draw(lastOpeningScan->candidateTriangles, candidate.firstTriangle, candidate.triangleCount,
				ImGui::GetColorU32(candidate.opening ? ImVec4(1.0f, 0.9f, 0.1f, 0.35f) : ImVec4(1.0f, 0.15f, 0.1f, 0.25f)));
	}
}

inline bool InteriorSun::IsInteriorWithSun(const RE::TESObjectCELL* cell)
{
	return cell && cell->cellFlags.all(RE::TESObjectCELL::Flag::kIsInteriorCell, RE::TESObjectCELL::Flag::kShowSky, RE::TESObjectCELL::Flag::kUseSkyLighting, static_cast<RE::TESObjectCELL::Flag>(CellFlagExt::kSunlightShadows));
}

RE::TESWorldSpace* InteriorSun::GetWorldSpace::thunk(RE::TES* tes)
{
	if (const auto cell = tes->interiorCell)
		return IsInteriorWithSun(cell) ? enableInteriorSun : disableInteriorSun;
	return func(tes);
}

RE::TESWorldSpace* InteriorSun::enableInteriorSun = [] {
	alignas(RE::TESWorldSpace) static char buffer[sizeof(RE::TESWorldSpace)]{};
	return reinterpret_cast<RE::TESWorldSpace*>(buffer);
}();

RE::TESWorldSpace* InteriorSun::disableInteriorSun = [] {
	alignas(RE::TESWorldSpace) static char buffer[sizeof(RE::TESWorldSpace)] = {};
	const auto noShadows = reinterpret_cast<RE::TESWorldSpace*>(buffer);
	noShadows->flags.set(RE::TESWorldSpace::Flag::kNoSky, RE::TESWorldSpace::Flag::kFixedDimensions);
	return noShadows;
}();

void InteriorSun::DirShadowLightCulling::thunk(RE::BSShadowDirectionalLight* dirLight, RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>& jobArrays, RE::BSTArray<RE::NiPointer<RE::NiAVObject>>& nodes)
{
	auto& singleton = globals::features::interiorSun;
	const auto cell = RE::TES::GetSingleton()->interiorCell;
	auto* passedJobArrays = &jobArrays;

	if (cell && singleton.isInteriorWithSun) {
		const auto* loadedData = cell->GetRuntimeData().loadedData;
		const auto portalGraph = loadedData ? loadedData->portalGraph : nullptr;
		if (portalGraph) {
			singleton.PopulateReplacementJobArrays(cell, portalGraph, dirLight, jobArrays);
			passedJobArrays = &singleton.replacementJobArrays;
		} else {
			singleton.currentCell = nullptr;
			singleton.currentPortalGraph = nullptr;
		}
	} else {
		if (!singleton.arraysCleared)
			singleton.ClearArrays();
		singleton.currentCell = nullptr;
		singleton.currentPortalGraph = nullptr;
	}

	func(dirLight, *passedJobArrays, nodes);
}

void InteriorSun::BSBatchRenderer_RenderPassImmediately::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	globals::features::interiorSun.UpdateRasterStateCullMode(a_pass, a_technique);
	func(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void InteriorSun::BSShaderAccumulator_StartAccumulating::thunk(RE::BSShaderAccumulator* a_accumulator, RE::NiCamera* a_camera)
{
	auto& singleton = globals::features::interiorSun;
	// Accumulators are also started from culling jobs, which must not touch the D3D context
	if (singleton.isInteriorWithSun && singleton.settings.WindowLightOnly && singleton.windowGlassVertexCount && std::this_thread::get_id() == singleton.renderThread)
		singleton.DrawWindowHoles(a_camera);
	func(a_accumulator, a_camera);
}

RE::BSEventNotifyControl InteriorSun::MenuOpenCloseEventHandler::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	if (a_event->menuName == RE::MainMenu::MENU_NAME) {
		if (a_event->opening)
			globals::features::interiorSun.isInteriorWithSun = false;
	}

	return RE::BSEventNotifyControl::kContinue;
}

void InteriorSun::ClearArrays()
{
	currentCellRoomsAndPortals.clear();

	for (auto& jobArray : replacementJobArrays)
		jobArray.clear();

	arraysCleared = true;
}

void InteriorSun::PopulateReplacementJobArrays(RE::TESObjectCELL* cell, const RE::NiPointer<RE::BSPortalGraph>& portalGraph, const RE::BSShadowDirectionalLight* dirLight, RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>& jobArrays)
{
	if (cell != currentCell || portalGraph != currentPortalGraph) {
		InitialiseOnNewCell(portalGraph);
		currentCell = cell;
		currentPortalGraph = portalGraph;
	}

	const auto jobArraySize = jobArrays.size();

	if (replacementJobArrays.size() != jobArraySize)
		replacementJobArrays.resize(jobArraySize);

	for (auto& jobArray : replacementJobArrays)
		jobArray.clear();

	addedSet.clear();

	// Copy the original job arrays contents into the replacement job arrays
	uint32_t count = 0;
	for (uint32_t i = 0; i < jobArraySize; ++i) {
		for (const auto& object : jobArrays[i]) {
			replacementJobArrays[i].push_back(object);
			addedSet.insert(object.get());
			count++;
		}
	}

	const auto playerPos = RE::PlayerCharacter::GetSingleton()->GetPosition();
	auto lightDir = -dirLight->GetShadowDirectionalLightRuntimeData().sunVector;
	lightDir.Unitize();

	// Add extra rooms and portals that are in the direction of the sun
	uint32_t added = 0;
	for (const auto& object : currentCellRoomsAndPortals) {
		if (addedSet.find(object.get()) != addedSet.end() || (!debugAllRoomsCast && !IsInSunDirectionAndWithinShadowDistance(object, lightDir, playerPos)))
			continue;

		addedSet.insert(object.get());
		replacementJobArrays[count++ % jobArraySize].push_back(object);
		++added;
	}
	roomsAddedLastFrame = added;
	roomsInCell = static_cast<uint32_t>(currentCellRoomsAndPortals.size());

	arraysCleared = false;
}

void InteriorSun::InitialiseOnNewCell(const RE::NiPointer<RE::BSPortalGraph>& portalGraph)
{
	currentCellRoomsAndPortals.clear();

	if (const auto portalSharedNode = portalGraph->portalSharedNode) {
		for (const auto room : portalGraph->rooms)
			currentCellRoomsAndPortals.push_back(room);

		for (auto child : portalGraph->portalSharedNode->GetChildren())
			currentCellRoomsAndPortals.push_back(child);
	}
}

bool InteriorSun::IsInSunDirectionAndWithinShadowDistance(const RE::NiPointer<RE::NiAVObject>& object, const RE::NiPoint3& lightDir, const RE::NiPoint3& playerPos) const
{
	const float radius = object->worldBound.radius;
	const auto diff = object->worldBound.center - playerPos;
	const float distance = diff.Length();
	const float projection = lightDir.Dot(diff);
	return projection >= -radius && (distance - radius) <= *gShadowDistance;
}

void InteriorSun::SetShadowDistance(bool inInterior)
{
	using func_t = decltype(SetShadowDistance);
	static REL::Relocation<func_t> func{ REL::RelocationID(98978, 105631).address() };
	func(inInterior);
}
#undef I18N_KEY_PREFIX
