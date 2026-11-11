#include "WindSolver.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace FluidFieldSim
{
	namespace
	{
		constexpr std::uint32_t GROUP_SIZE = 8;  // FLUIDFIELD_GROUP_SIZE in Common.hlsli
		constexpr std::uint32_t MAX_SRVS = 11;   // Per pass; t7 is the wall map
		constexpr std::uint32_t WALL_SLOT = 7;
		constexpr std::uint32_t MAX_UAVS = 8;

		ID3D11ShaderResourceView* const NULL_SRVS[MAX_SRVS] = {};
		ID3D11UnorderedAccessView* const NULL_UAVS[MAX_UAVS] = {};
	}

	WindSolver::Layer WindSolver::CreateLayer(ID3D11Device* device, std::uint32_t a_resolution, DXGI_FORMAT format, const NameFn& name, const std::string& label)
	{
		D3D11_TEXTURE2D_DESC desc{
			.Width = a_resolution,
			.Height = a_resolution,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = format,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};

		Layer layer;
		winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, layer.texture.put()));
		winrt::check_hresult(device->CreateShaderResourceView(layer.texture.get(), nullptr, layer.srv.put()));
		winrt::check_hresult(device->CreateUnorderedAccessView(layer.texture.get(), nullptr, layer.uav.put()));

		const std::string resourceName = "FluidField::" + label;
		name(layer.texture.get(), resourceName.c_str());
		name(layer.srv.get(), (resourceName + " SRV").c_str());
		name(layer.uav.get(), (resourceName + " UAV").c_str());
		return layer;
	}

	void WindSolver::CreateResources(ID3D11Device* device, std::uint32_t a_resolution, const NameFn& name, const char* a_label)
	{
		ReleaseResources();

		const std::string label = a_label;
		velocity[0] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32G32_FLOAT, name, label + "Velocity0");
		velocity[1] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32G32_FLOAT, name, label + "Velocity1");
		pressure[0] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32_FLOAT, name, label + "Pressure0");
		pressure[1] = CreateLayer(device, a_resolution, DXGI_FORMAT_R32_FLOAT, name, label + "Pressure1");
		divergence = CreateLayer(device, a_resolution, DXGI_FORMAT_R32_FLOAT, name, label + "Divergence");
		fog[0] = CreateLayer(device, a_resolution, DXGI_FORMAT_R16G16_FLOAT, name, label + "Fog0");
		fog[1] = CreateLayer(device, a_resolution, DXGI_FORMAT_R16G16_FLOAT, name, label + "Fog1");
		// Typed UAV atomics need single-channel 32-bit integers
		const char* splatNames[] = { "WindX", "WindY", "ShoveX", "ShoveY", "Carve", "ShoveX1", "ShoveY1", "Carve1" };
		for (std::size_t i = 0; i < splat.size(); i++)
			splat[i] = CreateLayer(device, a_resolution, i == 4 || i == 7 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R32_SINT, name, label + "Splat" + splatNames[i]);

		D3D11_BUFFER_DESC cbDesc{
			.ByteWidth = sizeof(Constants),
			.Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
			.MiscFlags = 0,
			.StructureByteStride = 0
		};
		winrt::check_hresult(device->CreateBuffer(&cbDesc, nullptr, constants.put()));
		name(constants.get(), ("FluidField::" + label + "Constants").c_str());

		cbDesc.ByteWidth = sizeof(FogParams);
		winrt::check_hresult(device->CreateBuffer(&cbDesc, nullptr, fogConstants.put()));
		name(fogConstants.get(), ("FluidField::" + label + "FogConstants").c_str());

		cbDesc.ByteWidth = sizeof(LevelConstants);
		winrt::check_hresult(device->CreateBuffer(&cbDesc, nullptr, levelConstants.put()));
		name(levelConstants.get(), ("FluidField::" + label + "LevelConstants").c_str());

		cbDesc.ByteWidth = sizeof(WallConstants);
		winrt::check_hresult(device->CreateBuffer(&cbDesc, nullptr, wallConstants.put()));
		name(wallConstants.get(), ("FluidField::" + label + "WallConstants").c_str());

		D3D11_BUFFER_DESC moverDesc{
			.ByteWidth = sizeof(Mover) * MAX_MOVERS,
			.Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
			.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
			.StructureByteStride = sizeof(Mover)
		};
		winrt::check_hresult(device->CreateBuffer(&moverDesc, nullptr, moverBuffer.put()));
		name(moverBuffer.get(), ("FluidField::" + label + "Movers").c_str());

		D3D11_SHADER_RESOURCE_VIEW_DESC moverSRVDesc{
			.Format = DXGI_FORMAT_UNKNOWN,
			.ViewDimension = D3D11_SRV_DIMENSION_BUFFER,
			.Buffer = { .FirstElement = 0, .NumElements = MAX_MOVERS }
		};
		winrt::check_hresult(device->CreateShaderResourceView(moverBuffer.get(), &moverSRVDesc, moverSRV.put()));
		name(moverSRV.get(), ("FluidField::" + label + "Movers SRV").c_str());

		resolution = a_resolution;
		current = 0;
		currentFog = 0;
		pressureResult = 0;
		historyValid = false;
	}

	bool WindSolver::CompileShaders(const CompileFn& compile)
	{
		ReleaseShaders();

		const std::pair<winrt::com_ptr<ID3D11ComputeShader>*, const wchar_t*> shaders[] = {
			{ &splatCS, L"SplatCS.hlsl" },
			{ &addForcesCS, L"AddForcesCS.hlsl" },
			{ &advectCS, L"AdvectCS.hlsl" },
			{ &divergenceCS, L"DivergenceCS.hlsl" },
			{ &jacobiCS, L"JacobiCS.hlsl" },
			{ &projectCS, L"ProjectCS.hlsl" },
			{ &fogCS, L"FogCS.hlsl" },
			{ &absorbCS, L"AbsorbCS.hlsl" },
		};

		for (auto& [program, fileName] : shaders) {
			if (auto shader = compile(fileName))
				program->attach(shader);
		}

		return HasShaders();
	}

	bool WindSolver::HasShaders() const
	{
		return splatCS && addForcesCS && advectCS && divergenceCS && jacobiCS && projectCS && fogCS && absorbCS;
	}

	void WindSolver::ReleaseShaders()
	{
		splatCS = nullptr;
		addForcesCS = nullptr;
		advectCS = nullptr;
		divergenceCS = nullptr;
		jacobiCS = nullptr;
		projectCS = nullptr;
		fogCS = nullptr;
		absorbCS = nullptr;
	}

	void WindSolver::ReleaseResources()
	{
		velocity = {};
		pressure = {};
		divergence = {};
		fog = {};
		splat = {};
		constants = nullptr;
		fogConstants = nullptr;
		levelConstants = nullptr;
		wallConstants = nullptr;
		moverBuffer = nullptr;
		moverSRV = nullptr;
		resolution = 0;
		historyValid = false;
	}

	void WindSolver::Reset(ID3D11DeviceContext* context)
	{
		if (!HasResources())
			return;

		const float zero[4] = {};
		for (auto* layer : { &velocity[0], &velocity[1], &pressure[0], &pressure[1], &divergence })
			context->ClearUnorderedAccessViewFloat(layer->uav.get(), zero);
		// Fog starts as the weather has it
		const float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		for (auto& layer : fog)
			context->ClearUnorderedAccessViewFloat(layer.uav.get(), one);

		historyValid = false;
	}

	void WindSolver::WriteLevel(ID3D11DeviceContext* context, const WindSolver* a_other, const WindSolver* a_child, std::uint32_t a_moverCount)
	{
		const bool hasOther = a_other && a_other->HasResources() && a_other->historyValid;
		const bool hasChild = a_child && a_child->HasResources() && a_child->historyValid;
		const LevelConstants data{
			.otherGridMin = { hasOther ? a_other->prevGridMin[0] : 0, hasOther ? a_other->prevGridMin[1] : 0 },
			.otherGridRes = hasOther ? a_other->resolution : 1u,
			.otherCellSize = hasOther ? a_other->cellSize : 1.0f,
			.childGridMin = { hasChild ? a_child->prevGridMin[0] : 0, hasChild ? a_child->prevGridMin[1] : 0 },
			.childGridRes = hasChild ? a_child->resolution : 1u,
			.childCellSize = hasChild ? a_child->cellSize : 1.0f,
			.hasOther = hasOther ? 1u : 0u,
			.hasChild = hasChild ? 1u : 0u,
			.moverCount = a_moverCount,
			.edgeBand = EDGE_BAND
		};
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(levelConstants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, &data, sizeof(data));
			context->Unmap(levelConstants.get(), 0);
		}
		ID3D11Buffer* cb = levelConstants.get();
		context->CSSetConstantBuffers(2, 1, &cb);
	}

	void WindSolver::BeginStep(ID3D11DeviceContext* context, const StepParams& params)
	{
		moverCount = static_cast<std::uint32_t>(std::min<std::size_t>(params.movers.size(), MAX_MOVERS));
		cellSize = params.cellSize;
		stepFog = params.fog != nullptr;

		Constants data{
			.gridMin = { params.gridMin[0], params.gridMin[1] },
			.prevGridMin = { historyValid ? prevGridMin[0] : params.gridMin[0], historyValid ? prevGridMin[1] : params.gridMin[1] },
			.gridRes = resolution,
			.cellSize = params.cellSize,
			.deltaTime = params.deltaTime,
			.velocityDecay = params.velocityDecay,
			.historyValid = historyValid ? 1u : 0u,
			.emitterCount = 0,
			.pad = {}
		};

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, &data, sizeof(data));
			context->Unmap(constants.get(), 0);
		}

		if (moverCount > 0 && SUCCEEDED(context->Map(moverBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, params.movers.data(), sizeof(Mover) * moverCount);
			context->Unmap(moverBuffer.get(), 0);
		}

		// The fog's inputs are read by the splat (the shove) as well as the fog pass
		const FogParams fogData = params.fog ? *params.fog : FogParams{};
		if (SUCCEEDED(context->Map(fogConstants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, &fogData, sizeof(fogData));
			context->Unmap(fogConstants.get(), 0);
		}

		ID3D11Buffer* cbs[] = { constants.get(), fogConstants.get() };
		context->CSSetConstantBuffers(0, 2, cbs);
		WriteLevel(context, params.parent, params.child, moverCount);

		const bool hasWalls = params.walls && params.walls->srv && params.walls->resolution > 0;
		const WallConstants walls{
			.groundMin = { hasWalls ? params.walls->min[0] : 0, hasWalls ? params.walls->min[1] : 0 },
			.groundRes = hasWalls ? params.walls->resolution : 1u,
			.groundCellSize = hasWalls ? params.walls->cellSize : 1.0f,
			.hasWalls = hasWalls ? 1u : 0u,
			.wallRise = hasWalls ? params.walls->rise : 0.0f,
			.pad = {}
		};
		if (SUCCEEDED(context->Map(wallConstants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, &walls, sizeof(walls));
			context->Unmap(wallConstants.get(), 0);
		}
		ID3D11Buffer* wallCB = wallConstants.get();
		context->CSSetConstantBuffers(3, 1, &wallCB);
		wallSRV = hasWalls ? params.walls->srv : nullptr;
		context->CSSetShaderResources(WALL_SLOT, 1, &wallSRV);

		prevGridMin[0] = params.gridMin[0];
		prevGridMin[1] = params.gridMin[1];
	}

	void WindSolver::EndStep(ID3D11DeviceContext* context)
	{
		historyValid = true;
		ID3D11Buffer* nullCBs[4] = {};
		context->CSSetShaderResources(0, MAX_SRVS, NULL_SRVS);
		context->CSSetShaderResources(WALL_SLOT, 1, NULL_SRVS);
		context->CSSetUnorderedAccessViews(0, MAX_UAVS, NULL_UAVS, nullptr);
		context->CSSetConstantBuffers(0, 4, nullCBs);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	void WindSolver::Dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, std::span<ID3D11ShaderResourceView* const> srvs, std::span<ID3D11UnorderedAccessView* const> uavs)
	{
		// Unbind the previous pass's outputs before they are read, or D3D11 silently nulls the new SRVs
		context->CSSetUnorderedAccessViews(0, MAX_UAVS, NULL_UAVS, nullptr);

		ID3D11ShaderResourceView* boundSRVs[MAX_SRVS] = {};
		std::copy_n(srvs.begin(), std::min<std::size_t>(srvs.size(), MAX_SRVS), boundSRVs);
		boundSRVs[WALL_SLOT] = wallSRV;
		context->CSSetShaderResources(0, MAX_SRVS, boundSRVs);
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::min<std::size_t>(uavs.size(), MAX_UAVS)), uavs.data(), nullptr);
		context->CSSetShader(shader, nullptr, 0);

		const std::uint32_t groups = (resolution + GROUP_SIZE - 1) / GROUP_SIZE;
		context->Dispatch(groups, groups, 1);
	}

	void WindSolver::Splat(ID3D11DeviceContext* context)
	{
		const UINT zero[4] = {};
		context->CSSetUnorderedAccessViews(0, MAX_UAVS, NULL_UAVS, nullptr);
		for (auto& layer : splat)
			context->ClearUnorderedAccessViewUint(layer.uav.get(), zero);
		if (moverCount == 0)
			return;

		// One group per mover, each drawing its own footprint: the cost follows the area moved through, not the grid
		ID3D11ShaderResourceView* srvs[] = { moverSRV.get() };
		ID3D11UnorderedAccessView* uavs[MAX_UAVS];
		for (std::size_t i = 0; i < splat.size(); i++)
			uavs[i] = splat[i].uav.get();
		context->CSSetShaderResources(0, 1, srvs);
		context->CSSetUnorderedAccessViews(0, MAX_UAVS, uavs, nullptr);
		context->CSSetShader(splatCS.get(), nullptr, 0);
		context->Dispatch(moverCount, 1, 1);
	}

	void WindSolver::AddForces(ID3D11DeviceContext* context, const WindSolver* parent)
	{
		const bool hasParent = parent && parent->HasResources();
		ID3D11ShaderResourceView* srvs[] = { velocity[current].srv.get(), splat[0].srv.get(), splat[1].srv.get(), hasParent ? parent->GetVelocitySRV() : nullptr };
		Dispatch(context, addForcesCS.get(), srvs, velocity[current ^ 1].uav.get());
		current ^= 1;
	}

	void WindSolver::Advect(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { velocity[current].srv.get() };
		Dispatch(context, advectCS.get(), srvs, velocity[current ^ 1].uav.get());
		current ^= 1;
	}

	void WindSolver::Divergence(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { velocity[current].srv.get() };
		Dispatch(context, divergenceCS.get(), srvs, divergence.uav.get());
	}

	void WindSolver::SolvePressure(ID3D11DeviceContext* context, std::uint32_t iterations)
	{
		// Start from zero every step, so pressure never carries data across a scroll
		context->CSSetUnorderedAccessViews(0, MAX_UAVS, NULL_UAVS, nullptr);
		const float zero[4] = {};
		context->ClearUnorderedAccessViewFloat(pressure[0].uav.get(), zero);

		std::uint32_t source = 0;
		for (std::uint32_t i = 0; i < iterations; i++) {
			ID3D11ShaderResourceView* srvs[] = { pressure[source].srv.get(), divergence.srv.get() };
			Dispatch(context, jacobiCS.get(), srvs, pressure[source ^ 1].uav.get());
			source ^= 1;
		}
		pressureResult = source;
	}

	void WindSolver::Project(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { velocity[current].srv.get(), pressure[pressureResult].srv.get() };
		Dispatch(context, projectCS.get(), srvs, velocity[current ^ 1].uav.get());
		current ^= 1;
	}

	void WindSolver::Fog(ID3D11DeviceContext* context, const WindSolver* parent)
	{
		const bool hasParent = parent && parent->HasResources();
		ID3D11ShaderResourceView* srvs[] = { velocity[current].srv.get(), fog[currentFog].srv.get(), splat[2].srv.get(), splat[3].srv.get(), splat[4].srv.get(),
			hasParent ? parent->GetFogSRV() : nullptr, nullptr, nullptr, splat[5].srv.get(), splat[6].srv.get(), splat[7].srv.get() };
		Dispatch(context, fogCS.get(), srvs, fog[currentFog ^ 1].uav.get());
		currentFog ^= 1;
	}

	void WindSolver::Absorb(ID3D11DeviceContext* context, const WindSolver& child)
	{
		if (!HasResources() || !historyValid || !child.HasResources() || !child.historyValid)
			return;

		ID3D11Buffer* cb = constants.get();
		context->CSSetConstantBuffers(0, 1, &cb);
		WriteLevel(context, &child, nullptr, 0);

		ID3D11ShaderResourceView* srvs[] = { velocity[current].srv.get(), fog[currentFog].srv.get(), child.GetVelocitySRV(), child.GetFogSRV() };
		ID3D11UnorderedAccessView* uavs[] = { velocity[current ^ 1].uav.get(), fog[currentFog ^ 1].uav.get() };
		Dispatch(context, absorbCS.get(), srvs, uavs);
		current ^= 1;
		currentFog ^= 1;

		ID3D11Buffer* nullCBs[3] = {};
		context->CSSetShaderResources(0, MAX_SRVS, NULL_SRVS);
		context->CSSetUnorderedAccessViews(0, MAX_UAVS, NULL_UAVS, nullptr);
		context->CSSetConstantBuffers(0, 3, nullCBs);
		context->CSSetShader(nullptr, nullptr, 0);
	}
}
