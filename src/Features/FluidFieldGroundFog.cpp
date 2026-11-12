#include "FluidFieldGroundFog.h"

#include "../I18n/I18n.h"
#include "Deferred.h"
#include "Features/LightLimitFix.h"
#include "Features/Skylighting.h"
#include "Features/VolumetricShadows.h"
#include "Features/WetnessEffects.h"
#include "FluidField/Field.h"
#include "Menu.h"
#include "Profiler.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#include <numbers>

#define I18N_KEY_PREFIX "feature.fluid_field_ground_fog."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FluidFieldGroundFog::Settings,
	Enabled,
	FogDensity,
	FogBrightness,
	GroundThickness,
	PoolDepth,
	EaseIn,
	FogDetail,
	ContactRadius,
	Stir,
	DistantReach,
	LocalLights,
	DebugView)

void FluidFieldGroundFog::DrawSettings()
{
	const auto tooltip = [](const char* a_text) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", a_text);
	};

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	tooltip(T(TKEY("enabled_tooltip"), "Fog lying on the ground around you. It drains downhill, pools in hollows and spills off ledges."));
	if (solverFailed || drawFailed)
		ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.Error, "%s", T(TKEY("shader_compile_failed"), "Ground Fog shaders failed to compile, so the fog is off. Check CommunityShaders.log."));

	ImGui::SliderFloat(T(TKEY("fog_density"), "Density"), &settings.FogDensity, 0.0f, 0.2f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_density_tooltip"), "How thick the fog is inside its layer."));
	ImGui::SliderFloat(T(TKEY("fog_brightness"), "Brightness"), &settings.FogBrightness, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_brightness_tooltip"), "How brightly the fog is lit by the sky and the sun."));
	ImGui::SliderFloat(T(TKEY("ground_thickness"), "Ground Thickness"), &settings.GroundThickness, 5.0f, 60.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("ground_thickness_tooltip"), "How deep fog lies on open ground where it forms, in world units."));
	ImGui::SliderFloat(T(TKEY("pool_depth"), "Pool Depth"), &settings.PoolDepth, 10.0f, 120.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("pool_depth_tooltip"), "How deep fog gathers in hollows, in world units."));
	ImGui::SliderFloat(T(TKEY("ease_in"), "Ease In"), &settings.EaseIn, 2.0f, 120.0f, "%.0f s", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("ease_in_tooltip"), "How long fog takes to form back where it was parted or drained away."));
	ImGui::SliderFloat(T(TKEY("fog_stir"), "Stirring"), &settings.Stir, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_stir_tooltip"), "How hard moving actors push the fog aside, piling it in front and leaving a wake."));

	if (ImGui::CollapsingHeader(T(TKEY("quality"), "Quality"))) {
		ImGui::PushID("QualitySection");
		static constexpr uint MIN_DETAIL = 0, MAX_DETAIL = 2;
		ImGui::SliderScalar(T(TKEY("fog_detail"), "Simulation Detail"), ImGuiDataType_U32, &settings.FogDetail, &MIN_DETAIL, &MAX_DETAIL, "%u", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("fog_detail_tooltip"), "How fine the simulated fog round you is. 0: finest, 43 m round you. 1: 58 m. 2: coarsest, 88 m. The cost stays the same. Past it, the distant (LOD) fog is always drawn."));
		ImGui::SliderFloat(T(TKEY("contact_radius"), "Contact Radius"), &settings.ContactRadius, 15.0f, 20.0f, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("contact_radius_tooltip"), "How far round you actors' legs cut exact holes in the fog. Farther out they only push it."));
		float reach = settings.DistantReach * 100.0f;
		if (ImGui::SliderFloat(T(TKEY("distant_reach"), "Distant Fog Reach"), &reach, 30.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
			settings.DistantReach = reach / 100.0f;
		tooltip(T(TKEY("distant_reach_tooltip"), "How far the distant fog reaches, as a share of the distant terrain's reach. It lies on the distant terrain, not on water."));
		static constexpr const char* LIGHT_TIERS[] = { "Off", "Nearest", "All" };
		int lights = static_cast<int>(settings.LocalLights);
		if (ImGui::Combo(T(TKEY("local_lights"), "Local Lights"), &lights, LIGHT_TIERS, IM_ARRAYSIZE(LIGHT_TIERS)))
			settings.LocalLights = static_cast<uint>(lights);
		tooltip(T(TKEY("local_lights_tooltip"), "Torches, fires and other lights glowing in the fog. Nearest: the few closest lights. All: every light in reach, costing more."));
		if (FluidFieldSim::LandscapeDepth::GetReach() > 0.0f)
			ImGui::Text(T(TKEY("distant_reach_now"), "Distant fog reaches %.0f m"), DistantDistance() / 70.0f);
		bool debugView = settings.DebugView == 1;
		if (ImGui::Checkbox(T(TKEY("debug_ground"), "Show Ground Map"), &debugView))
			settings.DebugView = debugView ? 1 : 0;
		tooltip(T(TKEY("debug_ground_tooltip"), "Colours every surface by what the fog takes it for: green ground fog may form on, red rock, yellow too steep, cyan water, grey not ground, blue not built yet (the loaded landscape). Striped: still being built. Brighter: fog lying there."));
		if (ImGui::Button(T(TKEY("reset_field"), "Reset Field")))
			queuedReset = true;
		ImGui::PopID();
	}
	if (IsActive())
		ImGui::Text(T(TKEY("ground_status"), "Ground: %zu tiles queued, %u provisional, %.3f ms a frame"), ground.GetQueued(), ground.GetProvisional(), groundMs);
}

#undef I18N_KEY_PREFIX

void FluidFieldGroundFog::LoadSettings(json& o_json)
{
	try {
		settings = o_json;
	} catch (const json::exception& e) {
		logger::warn("[Fluid Field] Malformed Ground Fog settings, using defaults: {}", e.what());
		settings = {};
	}
	const Settings defaults{};
	const auto clamp = [](float a_value, float a_min, float a_max, float a_default) { return std::isfinite(a_value) ? std::clamp(a_value, a_min, a_max) : a_default; };
	settings.FogDensity = clamp(settings.FogDensity, 0.0f, 0.2f, defaults.FogDensity);
	settings.FogBrightness = clamp(settings.FogBrightness, 0.0f, 2.0f, defaults.FogBrightness);
	settings.GroundThickness = clamp(settings.GroundThickness, 5.0f, 60.0f, defaults.GroundThickness);
	settings.PoolDepth = clamp(settings.PoolDepth, 10.0f, 120.0f, defaults.PoolDepth);
	settings.EaseIn = clamp(settings.EaseIn, 2.0f, 120.0f, defaults.EaseIn);
	settings.FogDetail = std::min(settings.FogDetail, 2u);
	settings.ContactRadius = clamp(settings.ContactRadius, 15.0f, 20.0f, defaults.ContactRadius);
	settings.Stir = clamp(settings.Stir, 0.0f, 3.0f, defaults.Stir);
	settings.DistantReach = clamp(settings.DistantReach, 0.3f, 1.0f, defaults.DistantReach);
	settings.LocalLights = std::min(settings.LocalLights, 2u);
	settings.DebugView = std::min(settings.DebugView, 1u);
}

void FluidFieldGroundFog::SaveSettings(json& o_json)
{
	o_json = settings;
}

void FluidFieldGroundFog::RestoreDefaultSettings()
{
	settings = {};
}

void FluidFieldGroundFog::PostPostLoad()
{
	FluidFieldSim::Field::Get().RegisterListeners();
}

void FluidFieldGroundFog::DataLoaded()
{
	FluidFieldSim::AirSources::Install();
	FluidFieldSim::LandscapeDepth::Install();
}

void FluidFieldGroundFog::ClearShaderCache()
{
	solver.ReleaseShaders();
	solverFailed = false;
	groundFogCS = nullptr;
	groundFogCompositeCS = nullptr;
	groundDebugCS = nullptr;
	drawFailed = false;
}

float FluidFieldGroundFog::DistantDistance() const
{
	return settings.DistantReach * FluidFieldSim::LandscapeDepth::GetReach();
}

void FluidFieldGroundFog::Release()
{
	ground.Release();
	FluidFieldSim::LandscapeDepth::SetEnabled(false);
	solver.ReleaseResources();
	solver.ReleaseShaders();
	cellSize = 0.0f;
	fogReady = false;
	groundFogCS = nullptr;
	groundFogCompositeCS = nullptr;
	groundDebugCS = nullptr;
	groundFogCB = nullptr;
	groundDebugCB = nullptr;
	linearSampler = nullptr;
	noiseSRVs = {};
	wrapSampler = nullptr;
	fogHalf.reset();
	fogHalfDistance.reset();
}

bool FluidFieldGroundFog::EnsureSolver()
{
	if (solverFailed)
		return false;
	if (!solver.HasResources()) {
		try {
			solver.CreateResources(globals::d3d::device, GRID_RESOLUTION, [](ID3D11DeviceChild* object, const char* name) { Util::SetResourceName(object, "%s", name); });
		} catch (const winrt::hresult_error& e) {
			logger::error("[Fluid Field] Ground fog could not create its textures: {}", winrt::to_string(e.message()));
			solverFailed = true;
			return false;
		}
	}
	if (!solver.HasShaders() && !solver.CompileShaders([](const wchar_t* fileName) {
			const auto path = std::filesystem::path("Data\\Shaders\\FluidField") / fileName;
			return reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0"));
		})) {
		logger::error("[Fluid Field] Ground fog solver shaders failed to compile; the fog stays off");
		solverFailed = true;
		return false;
	}
	return true;
}

FluidFieldSim::FogGround FluidFieldGroundFog::MakeGround() const
{
	FluidFieldSim::FogGround out;
	out.tiles = ground.GetSRV();
	out.land = ground.GetLandSRV();
	out.tileMin[0] = ground.GetTileMin()[0];
	out.tileMin[1] = ground.GetTileMin()[1];
	std::memcpy(out.slots.data(), ground.GetSlots().data(), sizeof(out.slots));
	out.landCell[0] = ground.GetLandCell()[0];
	out.landCell[1] = ground.GetLandCell()[1];
	return out;
}

FluidFieldSim::FogLayerParams FluidFieldGroundFog::MakeLayer() const
{
	FluidFieldSim::FogLayerParams layer;
	layer.groundThickness = settings.GroundThickness;
	layer.poolDepth = std::max(settings.PoolDepth, settings.GroundThickness);
	layer.sourceRate = 1.0f / settings.EaseIn;
	// The weather's wind: speed and direction stored as 0-255; direction 0 blows from the north
	if (const auto* sky = RE::Sky::GetSingleton(); sky && sky->currentWeather) {
		// Any real wind carries it visibly (square root, from a floor); only a near calm leaves it still
		const float wind = static_cast<float>(sky->currentWeather->data.windSpeed) / 255.0f;
		const float speed = wind < CALM_WIND ? 0.0f : std::lerp(MIN_WIND_DRIFT, MAX_WIND_DRIFT, std::sqrt(wind));
		const float heading = static_cast<float>(sky->currentWeather->data.windDirection) / 256.0f * 2.0f * std::numbers::pi_v<float>;
		layer.wind[0] = -std::sin(heading) * speed;
		layer.wind[1] = -std::cos(heading) * speed;
	}
	layer.time = fogTime;
	// How hard it rains, from Wetness Effects (its puddle ripples read the same)
	if (globals::features::wetnessEffects.loaded) {
		const auto wetness = globals::features::wetnessEffects.GetCommonBufferData();
		layer.rain = std::clamp(wetness.Raining, 0.0f, 1.0f);
		// Rain only where the sky is open: the precipitation occlusion map, as Wetness Effects and Skylighting read it
		auto& occlusion = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
		if (layer.rain > 0.0f && occlusion.depthSRV) {
			layer.occlusion = occlusion.depthSRV;
			std::memcpy(layer.occlusionViewProj, &wetness.OcclusionViewProj, sizeof(layer.occlusionViewProj));
			const auto eye = Util::GetEyePosition();
			layer.eye[0] = eye.x;
			layer.eye[1] = eye.y;
			layer.eye[2] = eye.z;
		}
	}
	return layer;
}

void FluidFieldGroundFog::Prepass()
{
	auto& field = FluidFieldSim::Field::Get();
	if (!settings.Enabled) {
		// Released once, so disabled costs nothing
		if (!released) {
			Release();
			released = true;
		}
		field.ReleaseIfUnused();
		return;
	}
	released = false;
	fogReady = false;

	const FluidFieldSim::Field::PrepassScope scope(*this);
	if (!scope.IsReady() || Util::IsInterior())
		return;

	// Every load leaves the player somewhere new
	if (field.ConsumeLoad(seenLoads)) {
		ground.Clear();
		queuedReset = true;
	}

	auto* context = globals::d3d::context;
	const auto playerPos = field.GetPlayerPosition();
	const auto start = std::chrono::steady_clock::now();
	globals::profiler->BeginPass("FluidField::Fog Ground");
	ground.Update(globals::d3d::device, context, field.GetLandscape(), field.GetWater(), playerPos.x, playerPos.y, field.GetDeltaTime());
	FluidFieldSim::LandscapeDepth::SetEnabled(true);
	globals::profiler->EndPass();
	const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	groundMs += (ms - groundMs) * 0.05f;

	const float deltaTime = field.GetDeltaTime();
	if (!ground.GetSRV() || !(deltaTime > 0.0f) || !EnsureSolver())
		return;

	// The grid centred on the player, snapped to whole cells; another cell size starts it again
	const float wantedCell = CELL_SIZES[settings.FogDetail];
	if (wantedCell != cellSize || queuedReset.exchange(false)) {
		cellSize = wantedCell;
		solver.Reset();
		airSources.Clear();
	}
	const int32_t half = static_cast<int32_t>(GRID_RESOLUTION / 2);
	gridMin[0] = static_cast<int32_t>(std::floor(playerPos.x / cellSize)) - half;
	gridMin[1] = static_cast<int32_t>(std::floor(playerPos.y / cellSize)) - half;
	const float step = std::min(deltaTime, 1.0f / 30.0f);
	fogTime += step;
	const FluidFieldSim::StepParams grid{ .gridMin = { gridMin[0], gridMin[1] }, .cellSize = cellSize, .deltaTime = step };

	// Moving actors push the fog; close by, their legs cut it (the contact map round the player)
	airSources.Update(playerPos, settings.ContactRadius * UNITS_PER_METRE, cellSize * static_cast<float>(half), step, settings.Stir, SHOVE_SPEED * settings.Stir, FluidFieldSim::FogSolver::MAX_MOVERS, ground.GetHeat());
	const int32_t contactHalf = static_cast<int32_t>(FluidFieldSim::FogSolver::CONTACT_RESOLUTION / 2);
	contactMin[0] = static_cast<int32_t>(std::floor(playerPos.x / FluidFieldSim::FogSolver::CONTACT_CELL)) - contactHalf;
	contactMin[1] = static_cast<int32_t>(std::floor(playerPos.y / FluidFieldSim::FogSolver::CONTACT_CELL)) - contactHalf;
	const FluidFieldSim::FogMovers movers{ .pushers = airSources.GetPushers(), .contacts = airSources.GetContacts(), .contactMin = { contactMin[0], contactMin[1] } };
	solver.Step(context, grid, MakeLayer(), MakeGround(), movers, [&](const char* a_pass, auto&& a_run) {
		globals::profiler->BeginPass(std::format("FluidField::{}", a_pass));
		a_run();
		globals::profiler->EndPass();
	});
	fogReady = true;
}

void FluidFieldGroundFog::DrawAfterWorld()
{
	if (!IsActive() || !fogReady || Util::IsInterior())
		return;
	auto* renderer = globals::game::renderer;
	// The targets the world was just rendered into; not always kMAIN, so the fog follows whatever is bound
	const auto& shadow = globals::game::shadowState->GetRuntimeData();
	const auto colorIndex = static_cast<uint32_t>(shadow.renderTargets[0]);
	const auto depthIndex = shadow.depthStencil;
	if (colorIndex >= RE::RENDER_TARGET::kTOTAL || depthIndex >= RE::RENDER_TARGET_DEPTHSTENCIL::kTOTAL)
		return;
	auto& main = renderer->GetRuntimeData().renderTargets[colorIndex];
	auto& depth = renderer->GetDepthStencilData().depthStencils[depthIndex];
	if (!main.UAV || !depth.depthSRV)
		return;

	// The live depth holds the water surfaces; it can only be read once it is no longer bound
	globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);
	if (!linearSampler) {
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(globals::d3d::device->CreateSamplerState(&samplerDesc, linearSampler.put())))
			return;
	}
	if (settings.DebugView == 1)
		DrawDebug(main.UAV, depth.depthSRV);
	else
		DrawFog(main.texture, main.UAV, depth.depthSRV);
	globals::game::shadowState->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

namespace
{
	/** @brief Binds the camera (b12) and shared data (b5, b6): earlier passes unbind or replace them. */
	void BindFrameBuffers(ID3D11DeviceContext* a_context)
	{
		auto* cameraSnapshot = FluidFieldSim::Field::Get().GetCameraSnapshot();
		ID3D11Buffer* perFrame = cameraSnapshot ? cameraSnapshot : *globals::game::perFrame;
		a_context->CSSetConstantBuffers(12, 1, &perFrame);
		ID3D11Buffer* sharedBuffers[2]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
		a_context->CSSetConstantBuffers(5, 2, sharedBuffers);
	}

	void UnbindAll(ID3D11DeviceContext* a_context)
	{
		ID3D11ShaderResourceView* nullSRVs[14]{};
		ID3D11UnorderedAccessView* nullUAVs[2]{};
		ID3D11SamplerState* nullSamplers[2]{};
		ID3D11Buffer* nullCB = nullptr;
		a_context->CSSetShaderResources(0, static_cast<UINT>(std::size(nullSRVs)), nullSRVs);
		a_context->CSSetShaderResources(14, 1, nullSRVs);
		a_context->CSSetShaderResources(18, 1, nullSRVs);
		a_context->CSSetShaderResources(35, 3, nullSRVs);
		a_context->CSSetShaderResources(98, 1, nullSRVs);
		a_context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
		a_context->CSSetSamplers(0, 2, nullSamplers);
		a_context->CSSetConstantBuffers(0, 1, &nullCB);
		a_context->CSSetConstantBuffers(12, 1, &nullCB);
		a_context->CSSetShader(nullptr, nullptr, 0);
	}

	template <class T>
	bool Upload(ID3D11DeviceContext* a_context, winrt::com_ptr<ID3D11Buffer>& a_buffer, const T& a_data)
	{
		if (!a_buffer) {
			const D3D11_BUFFER_DESC desc{ .ByteWidth = sizeof(T), .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE, .MiscFlags = 0, .StructureByteStride = 0 };
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, a_buffer.put())))
				return false;
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(a_context->Map(a_buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		std::memcpy(mapped.pData, &a_data, sizeof(T));
		a_context->Unmap(a_buffer.get(), 0);
		return true;
	}
}

bool FluidFieldGroundFog::EnsureNoise()
{
	if (noiseSRVs[0] && noiseSRVs[1] && noiseSRVs[2] && wrapSampler)
		return true;
	// Tiling value noise: a lattice of a_cells a side that wraps, eased between lattice points, a seed apiece
	const auto hash = [](uint32_t a_x, uint32_t a_y, uint32_t a_z, uint32_t a_cells, uint32_t a_seed) {
		uint32_t h = (a_x % a_cells) * 374761393u + (a_y % a_cells) * 668265263u + (a_z % a_cells) * 2147483647u + a_seed * 2246822519u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
	};
	const auto ease = [](float t) { return t * t * (3.0f - 2.0f * t); };
	const auto value = [&](uint32_t a_size, uint32_t a_cells, uint32_t a_seed, uint32_t x, uint32_t y, uint32_t z) {
		const uint32_t perCell = a_size / a_cells;
		const uint32_t cx = x / perCell, cy = y / perCell, cz = z / perCell;
		const float fx = ease((x % perCell + 0.5f) / perCell), fy = ease((y % perCell + 0.5f) / perCell), fz = ease((z % perCell + 0.5f) / perCell);
		const auto plane = [&](uint32_t a_z) {
			const float south = std::lerp(hash(cx, cy, a_z, a_cells, a_seed), hash(cx + 1, cy, a_z, a_cells, a_seed), fx);
			const float north = std::lerp(hash(cx, cy + 1, a_z, a_cells, a_seed), hash(cx + 1, cy + 1, a_z, a_cells, a_seed), fx);
			return std::lerp(south, north, fy);
		};
		return std::lerp(plane(cz), plane(cz + 1), fz);
	};
	auto* device = globals::d3d::device;
	// Detail and clumps: 3D, 64³, 8 and 4 lattice cells a side
	constexpr uint32_t SIZE_3D = 64;
	for (uint32_t k = 0; k < 2; k++) {
		const uint32_t cells = k == 0 ? 8 : 4;
		std::vector<uint8_t> texels(SIZE_3D * SIZE_3D * SIZE_3D);
		for (uint32_t z = 0; z < SIZE_3D; z++)
			for (uint32_t y = 0; y < SIZE_3D; y++)
				for (uint32_t x = 0; x < SIZE_3D; x++)
					texels[(z * SIZE_3D + y) * SIZE_3D + x] = static_cast<uint8_t>(std::clamp(value(SIZE_3D, cells, k + 1, x, y, z), 0.0f, 1.0f) * 255.0f + 0.5f);
		const D3D11_TEXTURE3D_DESC desc{ .Width = SIZE_3D, .Height = SIZE_3D, .Depth = SIZE_3D, .MipLevels = 1, .Format = DXGI_FORMAT_R8_UNORM, .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = 0 };
		const D3D11_SUBRESOURCE_DATA data{ .pSysMem = texels.data(), .SysMemPitch = SIZE_3D, .SysMemSlicePitch = SIZE_3D * SIZE_3D };
		winrt::com_ptr<ID3D11Texture3D> texture;
		if (FAILED(device->CreateTexture3D(&desc, &data, texture.put())) || FAILED(device->CreateShaderResourceView(texture.get(), nullptr, noiseSRVs[k].put())))
			return false;
		Util::SetResourceName(texture.get(), k == 0 ? "FluidField::GroundFogNoise Detail" : "FluidField::GroundFogNoise Clumps");
	}
	// Banks: 2D, 256², 8 lattice cells and an octave of 32
	constexpr uint32_t SIZE_2D = 256;
	std::vector<uint8_t> banks(SIZE_2D * SIZE_2D);
	for (uint32_t y = 0; y < SIZE_2D; y++)
		for (uint32_t x = 0; x < SIZE_2D; x++)
			banks[y * SIZE_2D + x] = static_cast<uint8_t>(std::clamp(value(SIZE_2D, 8, 3, x, y, 0) * 0.7f + value(SIZE_2D, 32, 4, x, y, 0) * 0.3f, 0.0f, 1.0f) * 255.0f + 0.5f);
	const D3D11_TEXTURE2D_DESC desc{ .Width = SIZE_2D, .Height = SIZE_2D, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R8_UNORM, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = 0 };
	const D3D11_SUBRESOURCE_DATA data{ .pSysMem = banks.data(), .SysMemPitch = SIZE_2D, .SysMemSlicePitch = 0 };
	winrt::com_ptr<ID3D11Texture2D> bankTexture;
	if (FAILED(device->CreateTexture2D(&desc, &data, bankTexture.put())) || FAILED(device->CreateShaderResourceView(bankTexture.get(), nullptr, noiseSRVs[2].put())))
		return false;
	Util::SetResourceName(bankTexture.get(), "FluidField::GroundFogNoise Banks");
	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	return SUCCEEDED(device->CreateSamplerState(&samplerDesc, wrapSampler.put()));
}

void FluidFieldGroundFog::DrawFog(ID3D11Texture2D* a_mainTexture, ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth)
{
	if (drawFailed || settings.FogDensity <= 0.0f || !EnsureNoise())
		return;
	auto* context = globals::d3d::context;
	if (!groundFogCS) {
		groundFogCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\GroundFogCS.hlsl", {}, "cs_5_0")));
		groundFogCompositeCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\GroundFogCS.hlsl", { { "COMPOSITE", "" } }, "cs_5_0")));
		if (!groundFogCS || !groundFogCompositeCS) {
			logger::error("[Fluid Field] Ground fog shader failed to compile; the ground fog stays off");
			groundFogCS = nullptr;
			groundFogCompositeCS = nullptr;
			drawFailed = true;
			return;
		}
	}

	// The lighting CS already has, where those features are in (Volumetric Shadows, Skylighting, Light Limit Fix)
	auto& volumetricShadows = globals::features::volumetricShadows;
	auto& skylighting = globals::features::skylighting;
	auto& lightLimitFix = globals::features::lightLimitFix;
	ID3D11ShaderResourceView* sunShadowSRV = volumetricShadows.loaded ? volumetricShadows.shadowCopySRV : nullptr;
	ID3D11ShaderResourceView* directionalShadowSRV = globals::deferred && globals::deferred->directionalShadowLights ? globals::deferred->directionalShadowLights->srv.get() : nullptr;
	ID3D11ShaderResourceView* skySRV = skylighting.loaded && skylighting.texProbeArray ? skylighting.texProbeArray->srv.get() : nullptr;
	ID3D11ShaderResourceView* lightSRVs[3] = {};
	if (lightLimitFix.loaded && lightLimitFix.lights && lightLimitFix.lightIndexList && lightLimitFix.lightGrid) {
		lightSRVs[0] = lightLimitFix.lights->srv.get();
		lightSRVs[1] = lightLimitFix.lightIndexList->srv.get();
		lightSRVs[2] = lightLimitFix.lightGrid->srv.get();
	}

	GroundFogConstants constants{
		.gridMin = { gridMin[0], gridMin[1] },
		.resolution = GRID_RESOLUTION,
		.cellSize = cellSize,
		.density = settings.FogDensity,
		.stepCount = STEP_COUNT,
		// To the loaded landscape's edge: settled fog past the simulated grid, fading out where Exponential Height Fog takes over
		.maxDistance = (FluidFieldSim::GroundTiles::LAND_REACH + 0.5f) * FluidFieldSim::LandscapeCache::CELL_UNITS,
		.maxDepth = std::max(settings.PoolDepth, settings.GroundThickness),
		.albedo = { 0.9f * settings.FogBrightness, 0.92f * settings.FogBrightness, 0.95f * settings.FogBrightness },
		.farDepth = settings.GroundThickness,
		.slotTiles = {},
		.tileMin = { ground.GetTileMin()[0], ground.GetTileMin()[1] },
		.landCell = { ground.GetLandCell()[0], ground.GetLandCell()[1] },
		.landPad = 0.0f,
		.hollowGain = MakeLayer().hollowGain,
		.farPoolDepth = std::max(settings.PoolDepth, settings.GroundThickness),
		.contactCell = FluidFieldSim::FogSolver::CONTACT_CELL,
		.contactMin = { contactMin[0], contactMin[1] },
		.contactResolution = FluidFieldSim::FogSolver::CONTACT_RESOLUTION,
		.farDistance = FluidFieldSim::LandscapeDepth::GetSRV() ? std::max(DistantDistance(), 12000.0f) : FAR_DISTANCE,
		.lodOrigin = {},
		.hasLod = FluidFieldSim::LandscapeDepth::GetSRV() ? 1u : 0u,
		.lodPad = 0.0f,
		.hasSunShadow = sunShadowSRV && directionalShadowSRV ? 1u : 0u,
		.hasSky = skySRV ? 1u : 0u,
		.localLights = lightSRVs[0] && lightSRVs[1] && lightSRVs[2] ? settings.LocalLights : 0u,
		.lightPad = 0.0f
	};
	std::memcpy(constants.slotTiles, ground.GetSlots().data(), sizeof(constants.slotTiles));
	if (!Upload(context, groundFogCB, constants))
		return;

	// Marched at half resolution (the nearest of each four pixels' surfaces), composited at full
	D3D11_TEXTURE2D_DESC mainDesc{};
	a_mainTexture->GetDesc(&mainDesc);
	const uint32_t halfWidth = (mainDesc.Width + 1) / 2, halfHeight = (mainDesc.Height + 1) / 2;
	if (!fogHalf || fogHalf->desc.Width != halfWidth || fogHalf->desc.Height != halfHeight) {
		const auto create = [&](DXGI_FORMAT a_format, const char* a_name) {
			D3D11_TEXTURE2D_DESC desc{ .Width = halfWidth, .Height = halfHeight, .MipLevels = 1, .ArraySize = 1, .Format = a_format, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, .CPUAccessFlags = 0, .MiscFlags = 0 };
			auto texture = std::make_unique<Texture2D>(desc, a_name);
			texture->CreateSRV({ .Format = a_format, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 } });
			texture->CreateUAV({ .Format = a_format, .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = 0 } });
			return texture;
		};
		fogHalf = create(DXGI_FORMAT_R16G16B16A16_FLOAT, "FluidField::GroundFogHalf");
		fogHalfDistance = create(DXGI_FORMAT_R32_FLOAT, "FluidField::GroundFogHalfDistance");
	}

	globals::profiler->BeginPass("FluidField::Ground Fog");
	ID3D11Buffer* cb = groundFogCB.get();
	context->CSSetConstantBuffers(0, 1, &cb);
	BindFrameBuffers(context);
	ID3D11ShaderResourceView* srvs[] = { a_depth, ground.GetSRV(), solver.GetColumnSRV(), nullptr, solver.GetBedSRV(), solver.GetContactLowSRV(), solver.GetContactHighSRV(), noiseSRVs[0].get(), nullptr, nullptr, noiseSRVs[1].get(), noiseSRVs[2].get(), ground.GetLandSRV(), FluidFieldSim::LandscapeDepth::GetSRV() };
	context->CSSetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);
	ID3D11SamplerState* samplers[] = { linearSampler.get(), wrapSampler.get() };
	context->CSSetSamplers(0, 2, samplers);
	context->CSSetShaderResources(14, 1, &skySRV);
	context->CSSetShaderResources(18, 1, &sunShadowSRV);
	context->CSSetShaderResources(35, 3, lightSRVs);
	context->CSSetShaderResources(98, 1, &directionalShadowSRV);
	ID3D11UnorderedAccessView* halfUAVs[] = { fogHalf->uav.get(), fogHalfDistance->uav.get() };
	context->CSSetUnorderedAccessViews(0, 2, halfUAVs, nullptr);
	context->CSSetShader(groundFogCS.get(), nullptr, 0);
	const auto dispatch = Util::GetScreenDispatchCount(true);
	context->Dispatch((dispatch.x + 1) / 2, (dispatch.y + 1) / 2, 1);

	ID3D11UnorderedAccessView* nullUAVs[2]{};
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	ID3D11ShaderResourceView* halfSRVs[] = { fogHalf->srv.get(), fogHalfDistance->srv.get() };
	context->CSSetShaderResources(8, 2, halfSRVs);
	context->CSSetUnorderedAccessViews(0, 1, &a_main, nullptr);
	context->CSSetShader(groundFogCompositeCS.get(), nullptr, 0);
	context->Dispatch(dispatch.x, dispatch.y, 1);
	UnbindAll(context);
	globals::profiler->EndPass();
}

void FluidFieldGroundFog::DrawDebug(ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth)
{
	if (drawFailed)
		return;
	auto* context = globals::d3d::context;
	if (!groundDebugCS) {
		groundDebugCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\GroundDebugCS.hlsl", {}, "cs_5_0")));
		if (!groundDebugCS) {
			logger::error("[Fluid Field] Ground fog debug shader failed to compile");
			drawFailed = true;
			return;
		}
	}

	GroundDebugConstants constants{};
	std::memcpy(constants.slotTiles, ground.GetSlots().data(), sizeof(constants.slotTiles));
	constants.tileMin[0] = ground.GetTileMin()[0];
	constants.tileMin[1] = ground.GetTileMin()[1];
	constants.landCell[0] = ground.GetLandCell()[0];
	constants.landCell[1] = ground.GetLandCell()[1];
	constants.resolution = GRID_RESOLUTION;
	constants.cellSize = cellSize;
	constants.poolDepth = std::max(settings.PoolDepth, settings.GroundThickness);
	constants.gridMin[0] = gridMin[0];
	constants.gridMin[1] = gridMin[1];
	if (!Upload(context, groundDebugCB, constants))
		return;

	globals::profiler->BeginPass("FluidField::Fog Debug");
	ID3D11Buffer* cb = groundDebugCB.get();
	context->CSSetConstantBuffers(0, 1, &cb);
	BindFrameBuffers(context);
	ID3D11ShaderResourceView* srvs[] = { a_depth, ground.GetSRV(), ground.GetLandSRV(), solver.GetColumnSRV() };
	context->CSSetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);
	context->CSSetUnorderedAccessViews(0, 1, &a_main, nullptr);
	context->CSSetShader(groundDebugCS.get(), nullptr, 0);
	const auto dispatch = Util::GetScreenDispatchCount(true);
	context->Dispatch(dispatch.x, dispatch.y, 1);
	UnbindAll(context);
	globals::profiler->EndPass();
}
