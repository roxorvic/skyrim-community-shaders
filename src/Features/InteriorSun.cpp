#include "InteriorSun.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/DebugSection.h"
#include "Utils/VersionedRelocation.h"

#define I18N_KEY_PREFIX "feature.interior_sun."

#include <numbers>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	InteriorSun::Settings,
	ForceDoubleSidedRendering,
	InteriorShadowDistance,
	WindowLightOnly)

namespace
{
	// Appends a shape's triangles in world space from its CPU geometry copy. False when the shape keeps none in a readable layout.
	bool AppendWorldTriangles(RE::BSGeometry* geometry, std::vector<float3>& vertices)
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
				return false;
			}
			const auto position = reinterpret_cast<const float*>(rendererData->rawVertexData + static_cast<size_t>(index) * stride);
			const auto world = geometry->world * RE::NiPoint3(position[0], position[1], position[2]);
			vertices.emplace_back(world.x, world.y, world.z);
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
}

void InteriorSun::DrawDebugSection()
{
	Util::DebugSection section;
	if (!section)
		return;

	ImGui::Text(T(TKEY("debug_window_glass"), "Window glass: %u shapes, %u triangles, %u unreadable shapes"), windowGlassShapes, windowGlassVertexCount / 3, windowGlassShapesSkipped);
	ImGui::Text(T(TKEY("debug_window_holes"), "Shadow maps given window holes last frame: %u (%u calls in total with another target bound)"), windowHolesDrawnLastFrame, windowHolesTargetMismatches);
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

	if (isInteriorWithSun && settings.WindowLightOnly)
		UpdateWindowGlass(cell);
	else if (windowGlassCell)
		ReleaseWindowGlass();

	windowHolesDrawnLastFrame = std::exchange(windowHolesDrawn, 0);
	renderThread = std::this_thread::get_id();
}

void InteriorSun::SetupResources()
{
	auto device = globals::d3d::device;

	windowHolesCB = new ConstantBuffer(ConstantBufferDesc<WindowHolesCB>(), "InteriorSun::WindowHolesCB");

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
}

void InteriorSun::ClearShaderCache()
{
	windowHolesVS = nullptr;
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
}

void InteriorSun::CollectWindowGlass(const RE::TESObjectCELL* cell)
{
	std::vector<float3> vertices;
	windowGlassShapes = 0;
	windowGlassShapesSkipped = 0;

	cell->ForEachReference([&](RE::TESObjectREFR* ref) {
		// Windows are statics; skipping everything else keeps the scan short
		const auto base = ref->GetBaseObject();
		const auto root = base && base->Is(RE::FormType::Static) ? ref->Get3D() : nullptr;
		if (root) {
			RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) {
				// The flag mesh authors put on window glass, which also keeps it out of forced double-sided rendering
				const auto property = geometry->GetGeometryRuntimeData().shaderProperty.get();
				if (property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kAssumeShadowmask))
					++(AppendWorldTriangles(geometry, vertices) ? windowGlassShapes : windowGlassShapesSkipped);
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
		return RE::BSContainer::ForEachResult::kContinue;
	});

	if (vertices == windowGlassVertices)
		return;

	windowGlassVertices = std::move(vertices);
	windowGlass.reset();
	windowGlassVertexCount = 0;
	logger::info("[Interior Sun] Window glass in cell {:08X}: {} shapes, {} triangles, {} unreadable shapes", cell->GetFormID(), windowGlassShapes, windowGlassVertices.size() / 3, windowGlassShapesSkipped);
	if (windowGlassVertices.empty())
		return;

	try {
		const uint64_t count = windowGlassVertices.size();
		D3D11_SUBRESOURCE_DATA init{ windowGlassVertices.data(), 0, 0 };
		windowGlass = std::make_unique<Buffer>(StructuredBufferDesc<float3>(count, false), &init, "InteriorSun::WindowGlass");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.NumElements = static_cast<UINT>(count);
		windowGlass->CreateSRV(srvDesc);
		windowGlassVertexCount = static_cast<uint32_t>(count);
	} catch (...) {
		logger::error("[Interior Sun] Failed to create the window glass buffer");
		windowGlass.reset();
	}
}

void InteriorSun::ReleaseWindowGlass()
{
	windowGlass.reset();
	windowGlassVertices.clear();
	windowGlassCell = nullptr;
	windowGlassCellFrames = 0;
	windowGlassVertexCount = 0;
	windowGlassShapes = 0;
	windowGlassShapesSkipped = 0;
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

	const auto vs = GetWindowHolesVS();
	if (!vs)
		return;

	D3D11_TEXTURE2D_DESC textureDesc;
	depthStencils[target].texture->GetDesc(&textureDesc);

	// The engine set this camera up right before, so these are the matrices its casters will use
	auto& shadowState = globals::game::shadowState->GetRuntimeData();
	const auto eye = shadowState.cameraData.getEye();
	const auto posAdjust = shadowState.posAdjust.getEye();

	WindowHolesCB data{};
	data.ViewProj = eye.viewProjMat;
	data.PosAdjust = { posAdjust.x, posAdjust.y, posAdjust.z };
	windowHolesCB->Update(data);

	// Same viewport as the engine derives from the camera's port (left, right, top, bottom)
	D3D11_VIEWPORT viewport{};
	viewport.TopLeftX = eye.viewPort[0] * textureDesc.Width;
	viewport.TopLeftY = (1.0f - eye.viewPort[2]) * textureDesc.Height;
	viewport.Width = (eye.viewPort[1] - eye.viewPort[0]) * textureDesc.Width;
	viewport.Height = (eye.viewPort[2] - eye.viewPort[3]) * textureDesc.Height;
	viewport.MaxDepth = 1.0f;

	// The engine caches its D3D state, so everything this draw changes is put back
	winrt::com_ptr<ID3D11InputLayout> oldInputLayout;
	D3D11_PRIMITIVE_TOPOLOGY oldTopology;
	winrt::com_ptr<ID3D11VertexShader> oldVS;
	winrt::com_ptr<ID3D11PixelShader> oldPS;
	winrt::com_ptr<ID3D11Buffer> oldCB;
	winrt::com_ptr<ID3D11ShaderResourceView> oldSRV;
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
	context->RSGetState(oldRasterState.put());
	context->RSGetViewports(&oldViewportCount, oldViewports);
	context->OMGetDepthStencilState(oldDepthState.put(), &oldStencilRef);

	// Everything is shadowed from the sun except through the panes; the casters drawn next still shadow that light
	context->ClearDepthStencilView(dsv.get(), D3D11_CLEAR_DEPTH, 0.0f, 0);

	auto cb = windowHolesCB->CB();
	auto srv = windowGlass->srv.get();
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->VSSetShader(vs, nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);
	context->VSSetConstantBuffers(0, 1, &cb);
	context->VSSetShaderResources(0, 1, &srv);
	context->RSSetState(windowHolesRasterState.get());
	context->RSSetViewports(1, &viewport);
	context->OMSetDepthStencilState(windowHolesDepthState.get(), 0);
	context->Draw(windowGlassVertexCount, 0);

	cb = oldCB.get();
	srv = oldSRV.get();
	context->IASetInputLayout(oldInputLayout.get());
	context->IASetPrimitiveTopology(oldTopology);
	context->VSSetShader(oldVS.get(), nullptr, 0);
	context->PSSetShader(oldPS.get(), nullptr, 0);
	context->VSSetConstantBuffers(0, 1, &cb);
	context->VSSetShaderResources(0, 1, &srv);
	context->RSSetState(oldRasterState.get());
	context->RSSetViewports(oldViewportCount, oldViewports);
	context->OMSetDepthStencilState(oldDepthState.get(), oldStencilRef);

	++windowHolesDrawn;
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
	if (singleton.isInteriorWithSun && singleton.windowGlassVertexCount && std::this_thread::get_id() == singleton.renderThread)
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
	for (const auto& object : currentCellRoomsAndPortals) {
		if (addedSet.find(object.get()) != addedSet.end() || !IsInSunDirectionAndWithinShadowDistance(object, lightDir, playerPos))
			continue;

		addedSet.insert(object.get());
		replacementJobArrays[count++ % jobArraySize].push_back(object);
	}

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
