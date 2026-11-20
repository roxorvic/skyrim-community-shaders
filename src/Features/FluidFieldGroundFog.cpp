#include "FluidFieldGroundFog.h"

#include "../I18n/I18n.h"
#include "Deferred.h"
#include "Features/LinearLighting.h"
#include "Features/Skylighting.h"
#include "Features/VolumetricLighting.h"
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
	Samples,
	DebugView,
	InteriorFogDensity,
	InteriorFogBrightness,
	InteriorGroundThickness,
	InteriorPoolDepth,
	InteriorEaseIn)

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

	if (ImGui::CollapsingHeader(T(TKEY("interiors"), "Interiors"))) {
		ImGui::PushID("InteriorsSection");
		ImGui::TextWrapped("%s", T(TKEY("interiors_hint"), "Dungeons, caves, mines and ruins use these instead of the values above. Each kind's own strength in GroundFogPlaces.json applies on top."));
		ImGui::SliderFloat(T(TKEY("interior_fog_density"), "Density"), &settings.InteriorFogDensity, 0.0f, 0.2f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("interior_fog_brightness"), "Brightness"), &settings.InteriorFogBrightness, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("interior_ground_thickness"), "Ground Thickness"), &settings.InteriorGroundThickness, 5.0f, 60.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("interior_pool_depth"), "Pool Depth"), &settings.InteriorPoolDepth, 10.0f, 120.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SliderFloat(T(TKEY("interior_ease_in"), "Ease In"), &settings.InteriorEaseIn, 2.0f, 120.0f, "%.0f s", ImGuiSliderFlags_AlwaysClamp);
		ImGui::PopID();
	}

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
		tooltip(T(TKEY("local_lights_tooltip"), "Torches, fires and other lights glowing in the fog. Nearest: the 4 lights closest to you. All: the 8 closest."));
		static constexpr uint MIN_SAMPLES = 12, MAX_SAMPLES = 64;
		ImGui::SliderScalar(T(TKEY("samples"), "Samples"), ImGuiDataType_U32, &settings.Samples, &MIN_SAMPLES, &MAX_SAMPLES, "%u", ImGuiSliderFlags_AlwaysClamp);
		tooltip(T(TKEY("samples_tooltip"), "Samples along each view ray through the fog. More: finer shadow edges and wakes, but the fog costs more, roughly in proportion."));
		if (FluidFieldSim::LandscapeDepth::GetReach() > 0.0f)
			ImGui::Text(T(TKEY("distant_reach_now"), "Distant fog reaches %.0f m"), DistantDistance() / 70.0f);
		ImGui::Checkbox(T(TKEY("debug_navmesh"), "Show Navmesh"), &showNavmesh);
		tooltip(T(TKEY("debug_navmesh_tooltip"), "Indoors, draws the cell's navmesh round you as lines (seen through walls): green level with your feet, blue below, yellow above. Fog lies on it."));
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
	settings.InteriorFogDensity = clamp(settings.InteriorFogDensity, 0.0f, 0.2f, defaults.InteriorFogDensity);
	settings.InteriorFogBrightness = clamp(settings.InteriorFogBrightness, 0.0f, 2.0f, defaults.InteriorFogBrightness);
	settings.InteriorGroundThickness = clamp(settings.InteriorGroundThickness, 5.0f, 60.0f, defaults.InteriorGroundThickness);
	settings.InteriorPoolDepth = clamp(settings.InteriorPoolDepth, 10.0f, 120.0f, defaults.InteriorPoolDepth);
	settings.InteriorEaseIn = clamp(settings.InteriorEaseIn, 2.0f, 120.0f, defaults.InteriorEaseIn);
	settings.FogDetail = std::min(settings.FogDetail, 2u);
	settings.ContactRadius = clamp(settings.ContactRadius, 15.0f, 20.0f, defaults.ContactRadius);
	settings.Stir = clamp(settings.Stir, 0.0f, 3.0f, defaults.Stir);
	settings.DistantReach = clamp(settings.DistantReach, 0.3f, 1.0f, defaults.DistantReach);
	settings.LocalLights = std::min(settings.LocalLights, 2u);
	settings.Samples = std::clamp(settings.Samples, 12u, 64u);
	settings.DebugView = std::min(settings.DebugView, 1u);
}

void FluidFieldGroundFog::DrawNavmeshOverlay()
{
	auto* tes = RE::TES::GetSingleton();
	auto* cell = tes ? tes->interiorCell : nullptr;
	auto* player = RE::PlayerCharacter::GetSingleton();
	auto* playerCamera = RE::PlayerCamera::GetSingleton();
	if (!showNavmesh || !cell || !player || !playerCamera || !playerCamera->cameraRoot)
		return;
	RE::NiCamera* camera = nullptr;
	for (auto& child : playerCamera->cameraRoot->GetChildren())
		if ((camera = netimmerse_cast<RE::NiCamera*>(child.get())))
			break;
	const auto* navmeshes = cell->GetRuntimeData().navMeshes;
	if (!camera || !navmeshes)
		return;
	constexpr float REACH = 2000.0f;
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const auto feet = player->GetPosition();
	auto* drawList = ImGui::GetBackgroundDrawList();
	for (const auto& mesh : navmeshes->navMeshes) {
		if (!mesh)
			continue;
		const auto& vertices = mesh->vertices;
		for (const auto& triangle : mesh->triangles) {
			if (triangle.vertices[0] >= vertices.size() || triangle.vertices[1] >= vertices.size() || triangle.vertices[2] >= vertices.size())
				continue;
			const RE::NiPoint3 corners[3] = { vertices[triangle.vertices[0]].location, vertices[triangle.vertices[1]].location, vertices[triangle.vertices[2]].location };
			const RE::NiPoint3 centre = (corners[0] + corners[1] + corners[2]) / 3.0f;
			if (centre.GetDistance(feet) > REACH)
				continue;
			ImVec2 points[3];
			bool visible = true;
			for (int k = 0; k < 3 && visible; k++) {
				float x = 0.0f, y = 0.0f, z = 0.0f;
				visible = camera->WorldPtToScreenPt3(corners[k], x, y, z, 1e-5f) && z > 0.0f;
				points[k] = ImVec2(x * display.x, (1.0f - y) * display.y);
			}
			if (!visible)
				continue;
			const float rise = centre.z - feet.z;
			const ImU32 colour = std::abs(rise) < 64.0f ? IM_COL32(40, 230, 60, 200) : rise < 0.0f ? IM_COL32(60, 120, 255, 200) :
			                                                                                         IM_COL32(255, 210, 40, 200);
			drawList->AddTriangle(points[0], points[1], points[2], colour, 1.0f);
		}
	}
}

FluidFieldGroundFog::Settings FluidFieldGroundFog::ActiveSettings() const
{
	Settings active = settings;
	if (currentPlace) {
		active.FogDensity = settings.InteriorFogDensity;
		active.FogBrightness = settings.InteriorFogBrightness;
		active.GroundThickness = settings.InteriorGroundThickness;
		active.PoolDepth = settings.InteriorPoolDepth;
		active.EaseIn = settings.InteriorEaseIn;
	}
	return active;
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
	LoadPlaces();
}

void FluidFieldGroundFog::LoadPlaces()
{
	places.clear();
	const auto path = Util::PathHelpers::GetCommunityShaderPath() / "FluidField" / "GroundFogPlaces.json";
	std::ifstream file(path);
	if (!file) {
		logger::warn("[Fluid Field] Ground fog: no {} (no fog indoors)", path.string());
		return;
	}
	try {
		const json data = json::parse(file);
		for (const auto& entry : data.at("dungeons")) {
			Place place;
			place.name = entry.value("name", std::string{});
			place.keywords = entry.at("keywords").get<std::vector<std::string>>();
			place.amount = std::clamp(entry.value("amount", 1.0f), 0.0f, 4.0f);
			place.density = std::clamp(entry.value("density", 1.0f), 0.0f, 4.0f);
			if (entry.contains("tint"))
				for (std::size_t i = 0; i < 3; i++)
					place.tint[i] = std::clamp(entry.at("tint").at(i).get<float>(), 0.0f, 2.0f);
			places.push_back(std::move(place));
		}
		logger::info("[Fluid Field] Ground fog: {} dungeon kinds", places.size());
	} catch (const std::exception& e) {
		places.clear();
		logger::warn("[Fluid Field] Ground fog: {} unreadable ({}): no fog indoors", path.string(), e.what());
	}
}

const FluidFieldGroundFog::Place* FluidFieldGroundFog::FindPlace() const
{
	auto* player = RE::PlayerCharacter::GetSingleton();
	const auto* location = player ? player->GetCurrentLocation() : nullptr;
	if (!location)
		return nullptr;
	for (const auto& place : places)
		for (const auto* at = location; at; at = at->parentLoc)
			for (const auto& keyword : place.keywords)
				if (at->HasKeywordString(keyword))
					return &place;
	return nullptr;
}

void FluidFieldGroundFog::ClearShaderCache()
{
	solver.ReleaseShaders();
	solverFailed = false;
	groundFogCS = nullptr;
	groundFogCompositeCS = nullptr;
	groundFogTemporalCS = nullptr;
	groundDebugCS = nullptr;
	fogTopCS = {};
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
	shadowSampler = nullptr;
	fogHalf.reset();
	fogHalfDistance.reset();
	fogHistory = {};
	fogHistoryDistance = {};
	historyValid = false;
	groundFogTemporalCS = nullptr;
	ReleaseFogTop();
}

void FluidFieldGroundFog::ReleaseFogTop()
{
	fogTopCS = {};
	drawAUAV = nullptr;
	drawBUAV = nullptr;
	lightFarUAV = nullptr;
	drawASRV = nullptr;
	drawBSRV = nullptr;
	lightFarSRV = nullptr;
	drawA = nullptr;
	drawB = nullptr;
	lightFar = nullptr;
	fogTopMipSRVs = {};
	fogTopMipUAVs = {};
	fogTopRawUAV = nullptr;
	fogTopRawSRV = nullptr;
	fogTopSRV = nullptr;
	fogTopRaw = nullptr;
	fogTop = nullptr;
}

bool FluidFieldGroundFog::EnsureFogTop()
{
	if (fogTop)
		return true;
	auto* device = globals::d3d::device;
	D3D11_TEXTURE2D_DESC desc{ .Width = TOP_TEXELS, .Height = TOP_TEXELS, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R32_FLOAT, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, .CPUAccessFlags = 0, .MiscFlags = 0 };
	bool ok = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, fogTopRaw.put())) && SUCCEEDED(device->CreateShaderResourceView(fogTopRaw.get(), nullptr, fogTopRawSRV.put())) && SUCCEEDED(device->CreateUnorderedAccessView(fogTopRaw.get(), nullptr, fogTopRawUAV.put()));
	desc.MipLevels = TOP_MIPS;
	ok = ok && SUCCEEDED(device->CreateTexture2D(&desc, nullptr, fogTop.put())) && SUCCEEDED(device->CreateShaderResourceView(fogTop.get(), nullptr, fogTopSRV.put()));
	for (uint32_t m = 0; ok && m < TOP_MIPS; m++) {
		const D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{ .Format = DXGI_FORMAT_R32_FLOAT, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { .MostDetailedMip = m, .MipLevels = 1 } };
		const D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{ .Format = DXGI_FORMAT_R32_FLOAT, .ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D, .Texture2D = { .MipSlice = m } };
		ok = SUCCEEDED(device->CreateShaderResourceView(fogTop.get(), &srvDesc, fogTopMipSRVs[m].put())) && SUCCEEDED(device->CreateUnorderedAccessView(fogTop.get(), &uavDesc, fogTopMipUAVs[m].put()));
	}
	D3D11_TEXTURE2D_DESC lightDesc{ .Width = GRID_RESOLUTION, .Height = GRID_RESOLUTION, .MipLevels = 1, .ArraySize = 2, .Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, .CPUAccessFlags = 0, .MiscFlags = 0 };
	ok = ok && SUCCEEDED(device->CreateTexture2D(&lightDesc, nullptr, drawA.put())) && SUCCEEDED(device->CreateShaderResourceView(drawA.get(), nullptr, drawASRV.put())) && SUCCEEDED(device->CreateUnorderedAccessView(drawA.get(), nullptr, drawAUAV.put()));
	ok = ok && SUCCEEDED(device->CreateTexture2D(&lightDesc, nullptr, drawB.put())) && SUCCEEDED(device->CreateShaderResourceView(drawB.get(), nullptr, drawBSRV.put())) && SUCCEEDED(device->CreateUnorderedAccessView(drawB.get(), nullptr, drawBUAV.put()));
	lightDesc.Width = lightDesc.Height = TOP_TEXELS;
	lightDesc.ArraySize = 1;
	lightDesc.Format = DXGI_FORMAT_R8G8_UNORM;
	ok = ok && SUCCEEDED(device->CreateTexture2D(&lightDesc, nullptr, lightFar.put())) && SUCCEEDED(device->CreateShaderResourceView(lightFar.get(), nullptr, lightFarSRV.put())) && SUCCEEDED(device->CreateUnorderedAccessView(lightFar.get(), nullptr, lightFarUAV.put()));
	if (!ok) {
		ReleaseFogTop();
		return false;
	}
	Util::SetResourceName(fogTopRaw.get(), "FluidField::FogTopRaw");
	Util::SetResourceName(fogTop.get(), "FluidField::FogTop");
	Util::SetResourceName(drawA.get(), "FluidField::FogDrawA");
	Util::SetResourceName(drawB.get(), "FluidField::FogDrawB");
	Util::SetResourceName(lightFar.get(), "FluidField::FogLightFar");
	return true;
}

void FluidFieldGroundFog::BuildFogTop(ID3D11DeviceContext* a_context)
{
	const auto pass = [&](ID3D11ComputeShader* a_shader, ID3D11ShaderResourceView* a_in, ID3D11UnorderedAccessView* a_out, uint32_t a_size) {
		ID3D11ShaderResourceView* nullSRV = nullptr;
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		a_context->CSSetShaderResources(16, 1, &a_in);
		a_context->CSSetUnorderedAccessViews(0, 1, &a_out, nullptr);
		a_context->CSSetShader(a_shader, nullptr, 0);
		a_context->Dispatch((a_size + 7) / 8, (a_size + 7) / 8, 1);
		a_context->CSSetShaderResources(16, 1, &nullSRV);
		a_context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	};
	pass(fogTopCS[0].get(), nullptr, fogTopRawUAV.get(), TOP_TEXELS);
	pass(fogTopCS[1].get(), fogTopRawSRV.get(), fogTopMipUAVs[0].get(), TOP_TEXELS);
	for (uint32_t m = 1; m < TOP_MIPS; m++)
		pass(fogTopCS[2].get(), fogTopMipSRVs[m - 1].get(), fogTopMipUAVs[m].get(), TOP_TEXELS >> m);
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
	const auto active = ActiveSettings();
	const float amount = currentPlace ? currentPlace->amount : 1.0f;
	layer.groundThickness = active.GroundThickness * amount;
	layer.poolDepth = std::max(active.PoolDepth, layer.groundThickness);
	layer.sourceRate = 1.0f / active.EaseIn;
	// The weather's wind: speed and direction stored as 0-255; direction 0 blows from the north
	if (const auto* sky = RE::Sky::GetSingleton(); sky && sky->currentWeather && !currentPlace) {
		// Any real wind carries it visibly (square root, from a floor); only a near calm leaves it still
		const float wind = static_cast<float>(sky->currentWeather->data.windSpeed) / 255.0f;
		const float speed = wind < CALM_WIND ? 0.0f : std::lerp(MIN_WIND_DRIFT, MAX_WIND_DRIFT, std::sqrt(wind));
		const float heading = static_cast<float>(sky->currentWeather->data.windDirection) / 256.0f * 2.0f * std::numbers::pi_v<float>;
		layer.wind[0] = -std::sin(heading) * speed;
		layer.wind[1] = -std::cos(heading) * speed;
	}
	layer.time = fogTime;
	// How hard it rains, from Wetness Effects (its puddle ripples read the same)
	if (globals::features::wetnessEffects.loaded && !currentPlace) {
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
	// Item 9: indoors only in dungeons (GroundFogPlaces.json); no distant fog indoors (the extra landscape draws stop)
	const bool interior = Util::IsInterior();
	currentPlace = interior ? FindPlace() : nullptr;
	if (interior)
		FluidFieldSim::LandscapeDepth::SetEnabled(false);
	if (!scope.IsReady() || (interior && !currentPlace))
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
	FluidFieldSim::LandscapeDepth::SetEnabled(!interior);
	// The distant fog fades in from 80% of the near fog's reach (GroundFogCS DISTANCE_FADE): landscape wholly inside that,
	// less a margin, is never read (item 8)
	FluidFieldSim::LandscapeDepth::SetNearSkip(0.8f * (FluidFieldSim::GroundTiles::LAND_REACH + 0.5f) * FluidFieldSim::LandscapeCache::CELL_UNITS - 512.0f);
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
	globals::profiler->BeginPass("FluidField::Fog Movers");
	airSources.Update(playerPos, settings.ContactRadius * UNITS_PER_METRE, cellSize * static_cast<float>(half), step, settings.Stir, SHOVE_SPEED * settings.Stir, FluidFieldSim::FogSolver::MAX_MOVERS, ground.GetHeat());
	globals::profiler->EndPass();
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
	if (!IsActive() || !fogReady || (Util::IsInterior() && !InDungeon()))
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
		ID3D11UnorderedAccessView* nullUAVs[3]{};
		ID3D11SamplerState* nullSamplers[3]{};
		ID3D11Buffer* nullCB = nullptr;
		a_context->CSSetShaderResources(0, static_cast<UINT>(std::size(nullSRVs)), nullSRVs);
		a_context->CSSetShaderResources(14, 10, nullSRVs);
		a_context->CSSetShaderResources(18, 1, nullSRVs);
		a_context->CSSetShaderResources(35, 3, nullSRVs);
		a_context->CSSetShaderResources(98, 1, nullSRVs);
		a_context->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
		a_context->CSSetSamplers(0, 3, nullSamplers);
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
	if (FAILED(device->CreateSamplerState(&samplerDesc, wrapSampler.put())))
		return false;
	samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
	return SUCCEEDED(device->CreateSamplerState(&samplerDesc, shadowSampler.put()));
}

void FluidFieldGroundFog::DrawFog(ID3D11Texture2D* a_mainTexture, ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth)
{
	const auto active = ActiveSettings();
	if (drawFailed || active.FogDensity <= 0.0f || !EnsureNoise())
		return;
	auto* context = globals::d3d::context;
	if (!groundFogCS) {
		groundFogCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\GroundFogCS.hlsl", {}, "cs_5_0")));
		groundFogCompositeCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\GroundFogCS.hlsl", { { "COMPOSITE", "" } }, "cs_5_0")));
		groundFogTemporalCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\GroundFogCS.hlsl", { { "TEMPORAL", "" } }, "cs_5_0")));
		constexpr const char* TOP_PASSES[] = { "0", "1", "2", "3" };
		for (int i = 0; i < 4; i++)
			fogTopCS[i].attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\GroundFogCS.hlsl", { { "TOP", TOP_PASSES[i] } }, "cs_5_0")));
		if (!groundFogCS || !groundFogCompositeCS || !groundFogTemporalCS || !fogTopCS[0] || !fogTopCS[1] || !fogTopCS[2] || !fogTopCS[3] || !EnsureFogTop()) {
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
	ID3D11ShaderResourceView* sunShadowSRV = volumetricShadows.loaded ? volumetricShadows.shadowCopySRV : nullptr;
	ID3D11ShaderResourceView* directionalShadowSRV = globals::deferred && globals::deferred->directionalShadowLights ? globals::deferred->directionalShadowLights->srv.get() : nullptr;
	// 4d: the game's sun shadow cascades (actors in them), as Skylighting captures them for its probes
	ID3D11ShaderResourceView* sceneShadowSRV = skylighting.loaded ? skylighting.shadowCascadeSRV : nullptr;
	ID3D11ShaderResourceView* skySRV = skylighting.loaded && skylighting.texProbeArray ? skylighting.texProbeArray->srv.get() : nullptr;
	const uint32_t lightCount = settings.LocalLights != 0 ? PickFogLights() : 0u;
	ID3D11ShaderResourceView* lightSRV = lightCount > 0 ? fogLights->SRV() : nullptr;

	// 4e: with Volumetric Lighting on, the fog's sun takes its colour, phase and strength (the weather's, as SkySync reads them)
	static const auto* volumetricLighting = reinterpret_cast<const RE::BSVolumetricLightingRenderData*>(REL::RelocationID(527719, 414629).address() - offsetof(RE::BSVolumetricLightingRenderData, color));
	const bool vlOn = globals::features::volumetricLighting.loaded && globals::game::bEnableVolumetricLighting && *globals::game::bEnableVolumetricLighting;

	// Last frame's fog is no use after a cut (a load, a teleport, a big jump)
	{
		const auto eye = Util::GetEyePosition();
		const float dx = eye.x - lastEye[0], dy = eye.y - lastEye[1], dz = eye.z - lastEye[2];
		if (dx * dx + dy * dy + dz * dz > 500.0f * 500.0f)
			historyValid = false;
		lastEye[0] = eye.x;
		lastEye[1] = eye.y;
		lastEye[2] = eye.z;
	}

	const std::array<float, 3> tint = currentPlace ? currentPlace->tint : std::array<float, 3>{ 1.0f, 1.0f, 1.0f };
	GroundFogConstants constants{
		.gridMin = { gridMin[0], gridMin[1] },
		.resolution = GRID_RESOLUTION,
		.cellSize = cellSize,
		.density = active.FogDensity * (currentPlace ? currentPlace->density : 1.0f),
		.stepCount = settings.Samples,
		// To the loaded landscape's edge: settled fog past the simulated grid, fading out where Exponential Height Fog takes over
		.maxDistance = (FluidFieldSim::GroundTiles::LAND_REACH + 0.5f) * FluidFieldSim::LandscapeCache::CELL_UNITS,
		.maxDepth = std::max(active.PoolDepth, active.GroundThickness),
		.albedo = { 0.9f * active.FogBrightness * tint[0], 0.92f * active.FogBrightness * tint[1], 0.95f * active.FogBrightness * tint[2] },
		.farDepth = active.GroundThickness * (currentPlace ? currentPlace->amount : 1.0f),
		.slotTiles = {},
		.tileMin = { ground.GetTileMin()[0], ground.GetTileMin()[1] },
		.landCell = { ground.GetLandCell()[0], ground.GetLandCell()[1] },
		.vlMix = vlOn ? volumetricLighting->customColor.contribution : 0.0f,
		.hollowGain = MakeLayer().hollowGain,
		.farPoolDepth = std::max(active.PoolDepth, active.GroundThickness),
		.contactCell = FluidFieldSim::FogSolver::CONTACT_CELL,
		.contactMin = { contactMin[0], contactMin[1] },
		.contactResolution = FluidFieldSim::FogSolver::CONTACT_RESOLUTION,
		.farDistance = FluidFieldSim::LandscapeDepth::GetSRV() ? std::max(DistantDistance(), 12000.0f) : FAR_DISTANCE,
		.vlPhase = { vlOn ? volumetricLighting->phaseFunction.scattering : 0.0f, vlOn ? volumetricLighting->phaseFunction.contribution : 0.0f },
		.hasLod = FluidFieldSim::LandscapeDepth::GetSRV() ? 1u : 0u,
		.vlIntensity = vlOn ? volumetricLighting->intensity : 0.0f,
		.hasSunShadow = sunShadowSRV && directionalShadowSRV && !currentPlace ? 1u : 0u,
		.hasSky = skySRV && !currentPlace ? 1u : 0u,
		.localLights = lightCount > 0 ? settings.LocalLights : 0u,
		.hasSceneShadow = sceneShadowSRV && directionalShadowSRV && !currentPlace ? 1u : 0u,
		.vlColour = { volumetricLighting->color.red, volumetricLighting->color.green, volumetricLighting->color.blue },
		.hasVL = vlOn && !currentPlace ? 1u : 0u,
		.hasHistory = historyValid ? 1u : 0u,
		.lightCount = lightCount,
		.historyPad = {}
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
		for (auto& history : fogHistory)
			history = create(DXGI_FORMAT_R16G16B16A16_FLOAT, "FluidField::GroundFogHistory");
		for (auto& history : fogHistoryDistance)
			history = create(DXGI_FORMAT_R32_FLOAT, "FluidField::GroundFogHistoryDistance");
		historyValid = false;
		constants.hasHistory = 0;
		if (!Upload(context, groundFogCB, constants))
			return;
	}

	ID3D11Buffer* cb = groundFogCB.get();
	context->CSSetConstantBuffers(0, 1, &cb);
	BindFrameBuffers(context);
	ID3D11ShaderResourceView* srvs[] = { a_depth, ground.GetSRV(), solver.GetColumnSRV(), ground.GetMaxSRV(), solver.GetBedSRV(), solver.GetContactLowSRV(), solver.GetContactHighSRV(), noiseSRVs[0].get(), nullptr, nullptr, noiseSRVs[1].get(), noiseSRVs[2].get(), ground.GetLandSRV(), FluidFieldSim::LandscapeDepth::GetSRV() };
	context->CSSetShaderResources(0, static_cast<UINT>(std::size(srvs)), srvs);
	ID3D11SamplerState* samplers[] = { linearSampler.get(), wrapSampler.get(), shadowSampler.get() };
	context->CSSetSamplers(0, 3, samplers);
	context->CSSetShaderResources(23, 1, &sceneShadowSRV);
	context->CSSetShaderResources(14, 1, &skySRV);
	context->CSSetShaderResources(18, 1, &sunShadowSRV);
	context->CSSetShaderResources(35, 1, &lightSRV);
	context->CSSetShaderResources(98, 1, &directionalShadowSRV);
	globals::profiler->BeginPass("FluidField::Fog Top");
	BuildFogTop(context);
	globals::profiler->EndPass();
	globals::profiler->BeginPass("FluidField::Fog Cells");
	ID3D11ShaderResourceView* drawFloorSRV = solver.GetDrawFloorSRV();
	context->CSSetShaderResources(22, 1, &drawFloorSRV);
	ID3D11UnorderedAccessView* cellUAVs[] = { drawAUAV.get(), lightFarUAV.get(), drawBUAV.get() };
	context->CSSetUnorderedAccessViews(0, 3, cellUAVs, nullptr);
	context->CSSetShader(fogTopCS[3].get(), nullptr, 0);
	context->Dispatch((GRID_RESOLUTION + 7) / 8, (GRID_RESOLUTION + 7) / 8, 1);
	ID3D11UnorderedAccessView* noUAVs[3]{};
	context->CSSetUnorderedAccessViews(0, 3, noUAVs, nullptr);
	globals::profiler->EndPass();
	globals::profiler->BeginPass("FluidField::Ground Fog");
	ID3D11ShaderResourceView* topSRV = fogTopSRV.get();
	context->CSSetShaderResources(15, 1, &topSRV);
	ID3D11ShaderResourceView* cellSRVs[] = { drawASRV.get(), lightFarSRV.get(), drawBSRV.get() };
	context->CSSetShaderResources(19, 3, cellSRVs);
	ID3D11UnorderedAccessView* halfUAVs[] = { fogHalf->uav.get(), fogHalfDistance->uav.get() };
	context->CSSetUnorderedAccessViews(0, 2, halfUAVs, nullptr);
	context->CSSetShader(groundFogCS.get(), nullptr, 0);
	const auto dispatch = Util::GetScreenDispatchCount(true);
	context->Dispatch((dispatch.x + 1) / 2, (dispatch.y + 1) / 2, 1);

	ID3D11UnorderedAccessView* nullUAVs[2]{};
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	// 4a/4b: blended over frames into this frame's history, which the composite lays over the scene
	const uint32_t previous = historyIndex, current = historyIndex ^ 1;
	ID3D11ShaderResourceView* nowSRVs[] = { fogHalf->srv.get(), fogHalfDistance->srv.get() };
	context->CSSetShaderResources(8, 2, nowSRVs);
	ID3D11ShaderResourceView* historySRVs[] = { fogHistory[previous]->srv.get(), fogHistoryDistance[previous]->srv.get() };
	context->CSSetShaderResources(24, 2, historySRVs);
	ID3D11UnorderedAccessView* historyUAVs[] = { fogHistory[current]->uav.get(), fogHistoryDistance[current]->uav.get() };
	context->CSSetUnorderedAccessViews(0, 2, historyUAVs, nullptr);
	context->CSSetShader(groundFogTemporalCS.get(), nullptr, 0);
	context->Dispatch((dispatch.x + 1) / 2, (dispatch.y + 1) / 2, 1);
	context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
	ID3D11ShaderResourceView* noHistory[2]{};
	context->CSSetShaderResources(24, 2, noHistory);
	historyIndex = current;
	historyValid = true;
	ID3D11ShaderResourceView* halfSRVs[] = { fogHistory[current]->srv.get(), fogHistoryDistance[current]->srv.get() };
	context->CSSetShaderResources(8, 2, halfSRVs);
	context->CSSetUnorderedAccessViews(0, 1, &a_main, nullptr);
	context->CSSetShader(groundFogCompositeCS.get(), nullptr, 0);
	context->Dispatch(dispatch.x, dispatch.y, 1);
	UnbindAll(context);
	globals::profiler->EndPass();
}

uint32_t FluidFieldGroundFog::PickFogLights()
{
	// The scene's active lights, shadow lights too (the fog takes no shadows, so none is dropped for lacking one), the
	// closest to the camera by the nearest point of their reach; the last few fade as the next one catches up, so none pops
	constexpr float HANDOVER = 256.0f;
	// Once a frame, from the main camera: the fog is also drawn for another view each frame, whose camera's pick flipped
	// the lights every other call
	if (lightPickFrame == globals::state->frameCount)
		return lightPickCount;
	lightPickFrame = globals::state->frameCount;
	lightPickCount = 0;
	auto* scene = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
	if (!scene)
		return 0;
	if (!fogLights) {
		fogLights = std::make_unique<StructuredBuffer>(StructuredBufferDesc<FogLight>(NEAR_LIGHTS), NEAR_LIGHTS, "FluidField::FogLights");
		fogLights->CreateSRV();
	}
	struct Candidate
	{
		float reach;
		FogLight light;
	};
	static std::vector<Candidate> candidates;
	candidates.clear();
	const auto eye = FluidFieldSim::Field::Get().GetSnapshotEye();
	const auto add = [&](const RE::NiPointer<RE::BSLight>& a_light, bool a_shadow) {
		auto* bsLight = a_light.get();
		auto* niLight = bsLight ? bsLight->light.get() : nullptr;
		if (!niLight || niLight->GetFlags().any(RE::NiAVObject::Flag::kHidden))
			return;
		const auto& runtime = niLight->GetLightRuntimeData();
		// A shadow light's dimmer is zeroed past the shadow distance: its own fade only
		const float fade = runtime.fade * (a_shadow ? 1.0f : bsLight->lodDimmer);
		const float radius = runtime.radius.x;
		const auto colour = globals::features::linearLighting.LightColorToWorking(niLight);
		if (radius <= 1e-4f || fade * (colour.red + colour.green + colour.blue) <= 1e-4f)
			return;
		const auto position = niLight->world.translate - eye;
		candidates.push_back({ position.Length() - radius, { { position.x, position.y, position.z }, radius, { colour.red * fade, colour.green * fade, colour.blue * fade }, 0.0f } });
	};
	for (const auto& light : scene->GetRuntimeData().activeLights)
		add(light, false);
	for (const auto& light : scene->GetRuntimeData().activeShadowLights)
		add(light, true);
	const std::size_t wanted = settings.LocalLights == 1 ? FEW_LIGHTS : NEAR_LIGHTS;
	const std::size_t kept = std::min(wanted, candidates.size());
	std::ranges::partial_sort(candidates, candidates.begin() + std::min(kept + 1, candidates.size()), {}, &Candidate::reach);
	const float next = candidates.size() > kept ? candidates[kept].reach : std::numeric_limits<float>::max();
	std::array<FogLight, NEAR_LIGHTS> picked{};
	for (std::size_t i = 0; i < kept; i++) {
		picked[i] = candidates[i].light;
		const float share = std::clamp((next - candidates[i].reach) / HANDOVER, 0.0f, 1.0f);
		for (float& channel : picked[i].colour)
			channel *= share;
	}
	fogLights->Update(picked.data(), sizeof(picked));
	lightPickCount = static_cast<uint32_t>(kept);
	return lightPickCount;
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
	constants.poolDepth = std::max(ActiveSettings().PoolDepth, ActiveSettings().GroundThickness);
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
