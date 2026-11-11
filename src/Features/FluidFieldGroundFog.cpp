#include "FluidFieldGroundFog.h"

#include "../I18n/I18n.h"
#include "FluidField/Field.h"
#include "Menu.h"
#include "Profiler.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/Game.h"

#define I18N_KEY_PREFIX "feature.fluid_field_ground_fog."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FluidFieldGroundFog::Settings,
	Enabled,
	DebugView)

void FluidFieldGroundFog::DrawSettings()
{
	const auto tooltip = [](const char* a_text) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", a_text);
	};

	ImGui::Checkbox(T(TKEY("enabled"), "Enabled"), &settings.Enabled);
	tooltip(T(TKEY("enabled_tooltip"), "Fog lying on the ground around you, in banks and clearings. It parts and swirls around moving actors and drifts with the weather's wind."));
	if (debugFailed)
		ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.Error, "%s", T(TKEY("shader_compile_failed"), "Ground Fog shaders failed to compile, so the fog is off. Check CommunityShaders.log."));

	bool debugView = settings.DebugView == 1;
	if (ImGui::Checkbox(T(TKEY("debug_ground"), "Show Ground Map"), &debugView))
		settings.DebugView = debugView ? 1 : 0;
	tooltip(T(TKEY("debug_ground_tooltip"), "Colours every surface by what the fog takes it for: green ground fog may form on, red rock, yellow too steep, cyan water, grey not ground, blue not built yet (the far height map). Striped: still being built."));
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

void FluidFieldGroundFog::ClearShaderCache()
{
	groundDebugCS = nullptr;
	debugFailed = false;
}

void FluidFieldGroundFog::Release()
{
	ground.Release();
	groundDebugCS = nullptr;
	groundDebugCB = nullptr;
	linearSampler = nullptr;
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
	if (!scope.IsReady() || Util::IsInterior())
		return;

	// Every load leaves the player somewhere new
	if (field.ConsumeLoad(seenLoads))
		ground.Clear();

	const auto start = std::chrono::steady_clock::now();
	globals::profiler->BeginPass("FluidField::Fog Ground");
	const auto playerPos = field.GetPlayerPosition();
	ground.Update(globals::d3d::device, globals::d3d::context, field.GetLandscape(), field.GetWater(), playerPos.x, playerPos.y, field.GetDeltaTime());
	globals::profiler->EndPass();
	const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	groundMs += (ms - groundMs) * 0.05f;
}

void FluidFieldGroundFog::DrawAfterWorld()
{
	if (!IsActive() || settings.DebugView == 0 || !ground.GetSRV() || Util::IsInterior())
		return;
	auto* renderer = globals::game::renderer;
	// The targets the world was just rendered into; not always kMAIN, so the view follows whatever is bound
	const auto& shadow = globals::game::shadowState->GetRuntimeData();
	const auto colorIndex = static_cast<uint32_t>(shadow.renderTargets[0]);
	const auto depthIndex = shadow.depthStencil;
	if (colorIndex >= RE::RENDER_TARGET::kTOTAL || depthIndex >= RE::RENDER_TARGET_DEPTHSTENCIL::kTOTAL)
		return;
	auto& main = renderer->GetRuntimeData().renderTargets[colorIndex];
	auto& depth = renderer->GetDepthStencilData().depthStencils[depthIndex];
	if (!main.UAV || !depth.depthSRV)
		return;

	// The live depth can only be read once it is no longer bound
	globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);
	DrawDebug(main.UAV, depth.depthSRV);
	globals::game::shadowState->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void FluidFieldGroundFog::DrawDebug(ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth)
{
	if (debugFailed)
		return;
	auto* device = globals::d3d::device;
	auto* context = globals::d3d::context;
	if (!groundDebugCS) {
		groundDebugCS.attach(reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\FluidField\\GroundDebugCS.hlsl", {}, "cs_5_0")));
		const D3D11_BUFFER_DESC desc{ .ByteWidth = sizeof(GroundDebugConstants), .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE, .MiscFlags = 0, .StructureByteStride = 0 };
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (!groundDebugCS || (!groundDebugCB && FAILED(device->CreateBuffer(&desc, nullptr, groundDebugCB.put()))) ||
			(!linearSampler && FAILED(device->CreateSamplerState(&samplerDesc, linearSampler.put())))) {
			logger::error("[Fluid Field] Ground fog debug shader failed to compile");
			groundDebugCS = nullptr;
			debugFailed = true;
			return;
		}
	}

	GroundDebugConstants constants{};
	std::memcpy(constants.slotTiles, ground.GetSlots().data(), sizeof(constants.slotTiles));
	constants.tileMin[0] = ground.GetTileMin()[0];
	constants.tileMin[1] = ground.GetTileMin()[1];
	const auto& farMap = ground.GetFarMap();
	constants.farScale[0] = farMap.scale[0];
	constants.farScale[1] = farMap.scale[1];
	constants.farOffset[0] = farMap.offset[0];
	constants.farOffset[1] = farMap.offset[1];
	constants.farZ[0] = farMap.z0;
	constants.farZ[1] = farMap.z1;
	constants.hasFar = ground.GetFarSRV() ? 1 : 0;
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(groundDebugCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return;
	std::memcpy(mapped.pData, &constants, sizeof(constants));
	context->Unmap(groundDebugCB.get(), 0);

	globals::profiler->BeginPass("FluidField::Fog Debug");
	ID3D11Buffer* cb = groundDebugCB.get();
	context->CSSetConstantBuffers(0, 1, &cb);
	// The camera (b12) and shared data (b5): earlier passes unbind or replace them
	auto* cameraSnapshot = FluidFieldSim::Field::Get().GetCameraSnapshot();
	ID3D11Buffer* perFrame = cameraSnapshot ? cameraSnapshot : *globals::game::perFrame;
	context->CSSetConstantBuffers(12, 1, &perFrame);
	ID3D11Buffer* sharedBuffers[2]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, sharedBuffers);
	ID3D11ShaderResourceView* srvs[3] = { a_depth, ground.GetSRV(), ground.GetFarSRV() };
	context->CSSetShaderResources(0, 3, srvs);
	ID3D11SamplerState* sampler = linearSampler.get();
	context->CSSetSamplers(0, 1, &sampler);
	context->CSSetUnorderedAccessViews(0, 1, &a_main, nullptr);
	context->CSSetShader(groundDebugCS.get(), nullptr, 0);
	const auto dispatch = Util::GetScreenDispatchCount(true);
	context->Dispatch(dispatch.x, dispatch.y, 1);

	ID3D11ShaderResourceView* nullSRVs[3]{};
	ID3D11UnorderedAccessView* nullUAV = nullptr;
	ID3D11SamplerState* nullSampler = nullptr;
	ID3D11Buffer* nullCB = nullptr;
	context->CSSetShaderResources(0, 3, nullSRVs);
	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	context->CSSetSamplers(0, 1, &nullSampler);
	context->CSSetConstantBuffers(0, 1, &nullCB);
	context->CSSetConstantBuffers(12, 1, &nullCB);
	context->CSSetShader(nullptr, nullptr, 0);
	globals::profiler->EndPass();
}
