#include "FluidFieldGroundFog.h"

#include "../I18n/I18n.h"
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
	FogThickness,
	FogStir,
	FogRefillSeconds,
	SimLevels,
	FineRadius,
	FarUpdateRate,
	JacobiIterations,
	WindDamping)

void FluidFieldGroundFog::DrawSettings()
{
	const auto tooltip = [](const char* a_text) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", a_text);
	};
	const auto errorText = [](const char* a_text) { ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.Error, "%s", a_text); };

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	tooltip(T(TKEY("enabled_tooltip"), "Fog lying on the ground around you, in banks and clearings. It parts and swirls around moving actors and drifts with the weather's wind."));
	if (shaderCompileFailed)
		errorText(T(TKEY("shader_compile_failed"), "Ground Fog shaders failed to compile, so the fog is off. Check CommunityShaders.log."));
	if (resourceCreateFailed)
		errorText(T(TKEY("resource_create_failed"), "Ground Fog could not create its textures, so the fog is off. Check CommunityShaders.log."));

	ImGui::SliderFloat(T(TKEY("fog_density"), "Density"), &settings.FogDensity, 0.0f, 0.2f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_density_tooltip"), "How thick the fog is inside its layer, in its banks."));
	ImGui::SliderFloat(T(TKEY("fog_brightness"), "Brightness"), &settings.FogBrightness, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_brightness_tooltip"), "How brightly the fog is lit by the sky and the sun."));
	ImGui::SliderFloat(T(TKEY("fog_thickness"), "Thickness"), &settings.FogThickness, 5.0f, 400.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_thickness_tooltip"), "How high the fog layer reaches above the ground, in world units. Thinner patches lie lower."));
	ImGui::SliderFloat(T(TKEY("fog_stir"), "Stirring"), &settings.FogStir, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_stir_tooltip"), "How hard actors shove the fog aside and push the air, piling it around them and swirling it behind them."));
	ImGui::SliderFloat(T(TKEY("fog_refill"), "Refill"), &settings.FogRefillSeconds, 0.5f, 30.0f, "%.1f s", ImGuiSliderFlags_AlwaysClamp);
	tooltip(T(TKEY("fog_refill_tooltip"), "How long shoved fog takes to settle back."));

	if (ImGui::CollapsingHeader(T(TKEY("quality"), "Quality"))) {
		ImGui::PushID("QualitySection");
		ImGui::SeparatorText(T(TKEY("wind"), "Wind and Fog Grid"));
		static constexpr uint MIN_LEVELS = 1, MAX_LEVELS = LEVEL_COUNT;
		ImGui::SliderScalar(T(TKEY("sim_levels"), "Levels"), ImGuiDataType_U32, &settings.SimLevels, &MIN_LEVELS, &MAX_LEVELS, "%u", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("sim_levels_tooltip"), "1: one medium grid around you. 2: plus a fine grid close by, where actors move by their skeleton, only while something moves in it. 3: plus a far, coarse grid round both."));
		if (settings.SimLevels >= 2) {
			ImGui::SliderFloat(T(TKEY("fine_radius"), "Fine Radius"), &settings.FineRadius, MIN_FINE_RADIUS, MAX_FINE_RADIUS, "%.0f", ImGuiSliderFlags_AlwaysClamp);
			tooltip(T(TKEY("fine_radius_tooltip"), "How far around you the fine grid reaches, in game units. Its cost stays the same: farther means coarser detail. Changing it clears the fine grid."));
			ImGui::Text(T(TKEY("cell_size"), "Fine cell size: %.1f units"), GetCellSize(0));
		}
		if (settings.SimLevels >= 3) {
			static constexpr uint MIN_RATE = 1, MAX_RATE = 4;
			ImGui::SliderScalar(T(TKEY("far_update_rate"), "Far Update Rate"), ImGuiDataType_U32, &settings.FarUpdateRate, &MIN_RATE, &MAX_RATE, "every %u frames", ImGuiSliderFlags_AlwaysClamp);
			tooltip(T(TKEY("far_update_rate_tooltip"), "How often the far grid steps. Distant fog moves slowly, so every few frames saves GPU time without showing."));
		}
		ImGui::SliderScalar(T(TKEY("jacobi_iterations"), "Pressure Iterations"), ImGuiDataType_U32, &settings.JacobiIterations, &MIN_JACOBI_ITERATIONS, &MAX_JACOBI_ITERATIONS, "%u", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("jacobi_iterations_tooltip"), "More iterations make the wind swirl more accurately and cost more GPU time."));
		ImGui::SliderFloat(T(TKEY("wind_damping"), "Wind Damping"), &settings.WindDamping, 0.0f, 5.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("wind_damping_tooltip"), "How quickly wind dies down. Higher values calm it sooner."));
		if (ImGui::Button(T(TKEY("reset_field"), "Reset Field")))
			queuedReset = true;
		ImGui::PopID();
	}
}

#undef I18N_KEY_PREFIX

FluidFieldGroundFog::Settings FluidFieldGroundFog::Sanitize(Settings a_settings)
{
	const Settings defaults{};

	a_settings.SimLevels = std::clamp(a_settings.SimLevels, 1u, LEVEL_COUNT);
	a_settings.FineRadius = std::isfinite(a_settings.FineRadius) ? std::clamp(a_settings.FineRadius, MIN_FINE_RADIUS, MAX_FINE_RADIUS) : defaults.FineRadius;
	a_settings.FarUpdateRate = std::clamp(a_settings.FarUpdateRate, 1u, 4u);
	a_settings.JacobiIterations = std::clamp(a_settings.JacobiIterations, MIN_JACOBI_ITERATIONS, MAX_JACOBI_ITERATIONS);
	a_settings.WindDamping = std::isfinite(a_settings.WindDamping) ? std::clamp(a_settings.WindDamping, 0.0f, 5.0f) : defaults.WindDamping;
	a_settings.FogDensity = std::isfinite(a_settings.FogDensity) ? std::clamp(a_settings.FogDensity, 0.0f, 0.2f) : defaults.FogDensity;
	a_settings.FogBrightness = std::isfinite(a_settings.FogBrightness) ? std::clamp(a_settings.FogBrightness, 0.0f, 2.0f) : defaults.FogBrightness;
	a_settings.FogThickness = std::isfinite(a_settings.FogThickness) ? std::clamp(a_settings.FogThickness, 5.0f, 400.0f) : defaults.FogThickness;
	a_settings.FogStir = std::isfinite(a_settings.FogStir) ? std::clamp(a_settings.FogStir, 0.0f, 3.0f) : defaults.FogStir;
	a_settings.FogRefillSeconds = std::isfinite(a_settings.FogRefillSeconds) ? std::clamp(a_settings.FogRefillSeconds, 0.5f, 30.0f) : defaults.FogRefillSeconds;
	return a_settings;
}

void FluidFieldGroundFog::LoadSettings(json& o_json)
{
	try {
		settings = o_json;
	} catch (const json::exception& e) {
		logger::warn("[Fluid Field] Malformed Ground Fog settings, using defaults: {}", e.what());
		settings = {};
	}
	settings = Sanitize(settings);
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

void FluidFieldGroundFog::ClearShaderCache()
{
	for (auto& level : levels)
		level.solver.ReleaseShaders();
	shaderCompileFailed = false;
	groundFogCS = nullptr;
	groundFogCompositeCS = nullptr;
	groundFogFailed = false;
}

bool FluidFieldGroundFog::EnsureSolver()
{
	if (shaderCompileFailed || resourceCreateFailed)
		return false;

	static constexpr const char* LABELS[LEVEL_COUNT] = { "Fine", "Medium", "Far" };
	for (uint i = 0; i < LEVEL_COUNT; i++) {
		auto& solver = levels[i].solver;
		if (solver.GetResolution() == LEVEL_RESOLUTION)
			continue;
		try {
			solver.CreateResources(globals::d3d::device, LEVEL_RESOLUTION, [](ID3D11DeviceChild* object, const char* name) { Util::SetResourceName(object, "%s", name); }, LABELS[i]);
		} catch (const winrt::hresult_error& e) {
			logger::error("[Fluid Field] Failed to create GPU resources: {}", winrt::to_string(e.message()));
			for (auto& level : levels)
				level.solver.ReleaseResources();
			resourceCreateFailed = true;
			return false;
		}
		// New textures hold undefined data
		queuedReset = true;
	}

	for (auto& level : levels) {
		if (level.solver.HasShaders())
			continue;
		const bool compiled = level.solver.CompileShaders([](const wchar_t* fileName) {
			const auto path = std::filesystem::path("Data\\Shaders\\FluidField") / fileName;
			return reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0"));
		});
		if (!compiled) {
			logger::error("[Fluid Field] Compute shaders failed to compile; the simulation stays off");
			for (auto& other : levels)
				other.solver.ReleaseShaders();
			shaderCompileFailed = true;
			return false;
		}
	}

	return true;
}

void FluidFieldGroundFog::Release()
{
	for (auto& level : levels) {
		level.solver.ReleaseResources();
		level.solver.ReleaseShaders();
		level.cellSize = 0.0f;
		level.weight = 0.0f;
		level.running = false;
	}
	groundHeight.Release();
	groundFogCS = nullptr;
	groundFogCompositeCS = nullptr;
	groundFogCB = nullptr;
	fogHalf.reset();
	fogHalfDistance.reset();
	fogReady = false;
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

	const FluidFieldSim::Field::PrepassScope scope(*this);
	if (!scope.IsReady())
		return;

	// Every load leaves the player somewhere new; start the wind from rest
	if (field.ConsumeLoad(seenLoads))
		queuedReset = true;

	if (!EnsureSolver())
		return;

	auto context = globals::d3d::context;

	if (queuedReset.exchange(false)) {
		ZoneScopedN("FluidField - Reset");
		TracyD3D11Zone(globals::state->tracyCtx, "FluidField - Reset");
		globals::profiler->BeginPass("FluidField::Reset");
		for (auto& level : levels)
			level.solver.Reset(context);
		airSources.Clear();
		fineIdle = 1e9f;
		globals::profiler->EndPass();
	}

	const float deltaTime = field.GetDeltaTime();
	if (!(deltaTime > 0.0f))
		return;
	frameCounter++;

	// Each level centred on the player, snapped to whole cells so texels never resample; grid coordinates are in cells, so
	// content from another cell size is meaningless
	const auto playerPos = field.GetPlayerPosition();
	const int32_t halfRes = static_cast<int32_t>(LEVEL_RESOLUTION / 2);
	float outerReach = 0.0f;
	for (uint i = 0; i < LEVEL_COUNT; i++) {
		auto& level = levels[i];
		const float cellSize = GetCellSize(i);
		if (cellSize != level.cellSize) {
			level.solver.Reset(context);
			level.cellSize = cellSize;
		}
		level.gridMin[0] = static_cast<int32_t>(std::floor(playerPos.x / cellSize)) - halfRes;
		level.gridMin[1] = static_cast<int32_t>(std::floor(playerPos.y / cellSize)) - halfRes;
		if (UsesLevel(i))
			outerReach = std::max(outerReach, cellSize * static_cast<float>(halfRes));
	}

	// Moving actors push the air (D21); by their skeleton only inside the fine level, short of its edge band
	const float fineReach = UsesLevel(0) ? GetCellSize(0) * static_cast<float>(halfRes - static_cast<int32_t>(FluidFieldSim::WindSolver::EDGE_BAND)) : 0.0f;
	const std::vector<FluidFieldSim::Mover>& movers = airSources.Update(playerPos, fineReach, outerReach, deltaTime, settings.FogStir, SHOVE_SPEED * settings.FogStir,
		FluidFieldSim::WindSolver::MAX_MOVERS);

	// The fine level runs only while something moves in it (and while it fades out after); restarted, it starts from the
	// medium level's state
	fineIdle = airSources.MovedNearby() ? 0.0f : fineIdle + deltaTime;
	auto& fine = levels[0];
	const bool fineWanted = UsesLevel(0) && fineIdle < FINE_IDLE_SECONDS;
	fine.weight = fineWanted ? std::min(fine.weight + deltaTime / FINE_FADE_IN_SECONDS, 1.0f) : std::max(fine.weight - deltaTime / FINE_FADE_OUT_SECONDS, 0.0f);
	const bool fineRuns = UsesLevel(0) && (fineWanted || fine.weight > 0.0f);
	if (fineRuns && !fine.running)
		fine.solver.Reset(context);
	fine.running = fineRuns;
	if (!fineRuns)
		fine.weight = 0.0f;
	auto& medium = levels[1];
	medium.running = true;
	medium.weight = 1.0f;
	auto& farLevel = levels[2];
	farLevel.running = UsesLevel(2);
	farLevel.weight = farLevel.running ? 1.0f : 0.0f;

	// The fog: carried by the wind and the weather's own wind, refilling behind actors
	fogParams = {};
	fogParams.refillRate = 1.0f / settings.FogRefillSeconds;
	fogParams.stirRate *= settings.FogStir;
	fogPatternOffset[0] += fogParams.drift[0] * deltaTime;
	fogPatternOffset[1] += fogParams.drift[1] * deltaTime;
	fogParams.patternOffset[0] = fogPatternOffset[0];
	fogParams.patternOffset[1] = fogPatternOffset[1];
	if (const auto* sky = RE::Sky::GetSingleton(); sky && sky->currentWeather) {
		// Wind speed and direction are stored as 0-255; direction 0 blows from the north
		const float speed = static_cast<float>(sky->currentWeather->data.windSpeed) / 255.0f * MAX_WEATHER_DRIFT;
		const float heading = static_cast<float>(sky->currentWeather->data.windDirection) / 256.0f * 2.0f * std::numbers::pi_v<float>;
		fogParams.drift[0] = -std::sin(heading) * speed;
		fogParams.drift[1] = -std::cos(heading) * speed;
	}

	// Coarse to fine (each takes its edges from the one round it), then each absorbs the finer one
	// Rocks, logs and trunks are walls on the fine and medium levels (the far level's cells are wider than a rock); from the
	// ground map's last fill
	const FluidFieldSim::WallMap walls{ .srv = groundHeight.GetSRV(), .min = { fogGroundMin[0], fogGroundMin[1] }, .resolution = FOG_GROUND_RESOLUTION, .cellSize = FOG_GROUND_CELL };
	const auto step = [&](uint a_index, const FluidFieldSim::WindSolver* a_parent, const FluidFieldSim::WindSolver* a_child, float a_deltaTime, uint a_iterations) {
		auto& level = levels[a_index];
		FluidFieldSim::StepParams params{
			.gridMin = { level.gridMin[0], level.gridMin[1] },
			.cellSize = level.cellSize,
			.deltaTime = a_deltaTime,
			.velocityDecay = std::exp(-settings.WindDamping * a_deltaTime),
			.jacobiIterations = std::max(a_iterations, 1u),
			.movers = movers,
			.fog = &fogParams,
			.parent = a_parent,
			.child = a_child,
			.walls = a_index < 2 && fogReady ? &walls : nullptr
		};
		static constexpr const char* NAMES[LEVEL_COUNT] = { "Fine", "Medium", "Far" };
		ZoneScopedN("FluidField - Wind Step");
		TracyD3D11Zone(globals::state->tracyCtx, "FluidField - Wind Step");
		level.solver.Step(context, params, [&](const char* passName, auto&& runPass) {
#ifdef TRACY_ENABLE
			ZoneTransientN(fluidFieldPassZone, passName, true);
			TracyD3D11ZoneTransientS(globals::state->tracyCtx, fluidFieldPassGpuZone, passName, 0, globals::state->tracyCtx != nullptr);
#endif
			// The built-in profiler cannot nest passes, so each solver pass is timed on its own
			globals::profiler->BeginPass(std::format("FluidField::{} {}", NAMES[a_index], passName));
			runPass();
			globals::profiler->EndPass();
		});
	};
	const auto* fineChild = fine.running ? &fine.solver : nullptr;
	if (farLevel.running) {
		farLevel.pendingTime += deltaTime;
		if (frameCounter % settings.FarUpdateRate == 0) {
			step(2, nullptr, &medium.solver, std::min(farLevel.pendingTime, 0.25f), settings.JacobiIterations / 2);
			farLevel.pendingTime = 0.0f;
		}
	}
	step(1, farLevel.running ? &farLevel.solver : nullptr, fineChild, deltaTime, settings.JacobiIterations * 3 / 4);
	if (fine.running)
		step(0, &medium.solver, nullptr, deltaTime, settings.JacobiIterations);
	globals::profiler->BeginPass("FluidField::Absorb");
	if (fine.running)
		medium.solver.Absorb(context, fine.solver);
	if (farLevel.running)
		farLevel.solver.Absorb(context, medium.solver);
	globals::profiler->EndPass();

	// The ground the fog lies on, wider than the simulated area
	fogReady = false;
	globals::profiler->BeginPass("FluidField::Fog Ground");
	const int32_t groundHalf = static_cast<int32_t>(FOG_GROUND_RESOLUTION / 2);
	fogGroundMin[0] = static_cast<int32_t>(std::floor(playerPos.x / FOG_GROUND_CELL)) - groundHalf;
	fogGroundMin[1] = static_cast<int32_t>(std::floor(playerPos.y / FOG_GROUND_CELL)) - groundHalf;
	fogReady = groundHeight.Update(globals::d3d::device, context, field.GetLandscape(), field.GetWater(), fogGroundMin, FOG_GROUND_RESOLUTION, FOG_GROUND_CELL);
	globals::profiler->EndPass();
}

void FluidFieldGroundFog::DrawAfterWorld()
{
	if (!fogReady)
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
	DrawGroundFog(main.texture, main.UAV, depth.depthSRV);
	globals::game::shadowState->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void FluidFieldGroundFog::DrawGroundFog(ID3D11Texture2D* a_mainTexture, ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth)
{
	if (!IsActive() || !fogReady || settings.FogDensity <= 0.0f || !a_mainTexture || !a_main || !a_depth || groundFogFailed)
		return;
	if (Util::IsInterior() || !levels[1].solver.HasResources() || !groundHeight.GetSRV())
		return;

	auto* context = globals::d3d::context;
	if (!groundFogCS) {
		groundFogCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\GroundFogCS.hlsl", {}, "cs_5_0")));
		groundFogCompositeCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\GroundFogCS.hlsl", { { "COMPOSITE", "" } }, "cs_5_0")));
		D3D11_BUFFER_DESC desc{ .ByteWidth = sizeof(GroundFogConstants), .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE, .MiscFlags = 0, .StructureByteStride = 0 };
		if (!groundFogCS || !groundFogCompositeCS || FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, groundFogCB.put()))) {
			logger::error("[Fluid Field] Ground fog shader failed to compile; the ground fog stays off");
			groundFogCS = nullptr;
			groundFogCompositeCS = nullptr;
			groundFogFailed = true;
			return;
		}
		Util::SetResourceName(groundFogCB.get(), "FluidField::GroundFogConstants");
	}

	GroundFogConstants constants{
		.levelGridMin = {},
		.levelInfo = {},
		.density = settings.FogDensity,
		.thickness = settings.FogThickness,
		.maxHeight = groundHeight.GetMaxHeight(),
		.stepCount = GROUND_FOG_STEPS,
		.albedo = { 0.9f * settings.FogBrightness, 0.92f * settings.FogBrightness, 0.95f * settings.FogBrightness },
		.maxDistance = 0.5f * FOG_GROUND_CELL * static_cast<float>(FOG_GROUND_RESOLUTION),
		.groundMin = { fogGroundMin[0], fogGroundMin[1] },
		.groundRes = FOG_GROUND_RESOLUTION,
		.groundCellSize = FOG_GROUND_CELL,
		.patternOffset = { fogPatternOffset[0], fogPatternOffset[1] },
		.debugMode = 0,
		.pad = 0.0f
	};
	for (uint i = 0; i < LEVEL_COUNT; i++) {
		const auto& level = levels[i];
		const bool shown = level.weight > 0.0f && level.solver.HasResources();
		constants.levelGridMin[i][0] = level.gridMin[0];
		constants.levelGridMin[i][1] = level.gridMin[1];
		constants.levelInfo[i][0] = std::max(level.cellSize, 1.0f);
		constants.levelInfo[i][1] = shown ? level.weight : 0.0f;
		constants.levelInfo[i][2] = static_cast<float>(LEVEL_RESOLUTION);
	}
	// D193: marched at half resolution (the nearest of each four pixels' surfaces), composited at full
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

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(groundFogCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return;
	std::memcpy(mapped.pData, &constants, sizeof(constants));
	context->Unmap(groundFogCB.get(), 0);

	globals::profiler->BeginPass("FluidField::Ground Fog");
	ID3D11Buffer* cb = groundFogCB.get();
	ID3D11ShaderResourceView* srvs[] = { a_depth, groundHeight.GetSRV(), levels[0].solver.GetFogSRV(), levels[1].solver.GetFogSRV(), levels[2].solver.GetFogSRV(),
		levels[0].solver.GetVelocitySRV(), levels[1].solver.GetVelocitySRV(), levels[2].solver.GetVelocitySRV() };
	context->CSSetConstantBuffers(0, 1, &cb);
	// Bind the camera (b12) and shared/feature data (b5, b6) explicitly: earlier passes unbind or replace them
	auto* cameraSnapshot = FluidFieldSim::Field::Get().GetCameraSnapshot();
	ID3D11Buffer* perFrame = cameraSnapshot ? cameraSnapshot : *globals::game::perFrame;
	context->CSSetConstantBuffers(12, 1, &perFrame);
	ID3D11Buffer* sharedBuffers[2]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, sharedBuffers);
	context->CSSetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);
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

	ID3D11ShaderResourceView* nullSRVs[10]{};
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetShaderResources(0, static_cast<UINT>(std::size(nullSRVs)), nullSRVs);
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullCB);
	context->CSSetConstantBuffers(12, 1, &nullCB);
	context->CSSetShader(nullptr, nullptr, 0);
	globals::profiler->EndPass();
}
