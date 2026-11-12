#include "FogSolver.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>

namespace FluidFieldSim
{
	FogSolver::Layer FogSolver::CreateLayer(ID3D11Device* device, std::uint32_t resolution, std::uint32_t slices, DXGI_FORMAT format, const NameFn& name, const char* label)
	{
		Layer layer;
		const D3D11_TEXTURE2D_DESC desc{ .Width = resolution, .Height = resolution, .MipLevels = 1, .ArraySize = slices, .Format = format, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, .CPUAccessFlags = 0, .MiscFlags = 0 };
		winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, layer.texture.put()));
		winrt::check_hresult(device->CreateShaderResourceView(layer.texture.get(), nullptr, layer.srv.put()));
		winrt::check_hresult(device->CreateUnorderedAccessView(layer.texture.get(), nullptr, layer.uav.put()));
		if (name) {
			const std::string base = std::string("FluidField::Fog ") + label;
			name(layer.texture.get(), base.c_str());
			name(layer.srv.get(), (base + " SRV").c_str());
			name(layer.uav.get(), (base + " UAV").c_str());
		}
		return layer;
	}

	FogSolver::Buffer FogSolver::CreateMoverBuffer(ID3D11Device* device, const NameFn& name, const char* label)
	{
		Buffer out;
		const D3D11_BUFFER_DESC desc{ .ByteWidth = sizeof(Mover) * MAX_MOVERS, .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE, .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, .StructureByteStride = sizeof(Mover) };
		winrt::check_hresult(device->CreateBuffer(&desc, nullptr, out.buffer.put()));
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		view.Format = DXGI_FORMAT_UNKNOWN;
		view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		view.Buffer.NumElements = MAX_MOVERS;
		winrt::check_hresult(device->CreateShaderResourceView(out.buffer.get(), &view, out.srv.put()));
		if (name)
			name(out.buffer.get(), (std::string("FluidField::Fog ") + label).c_str());
		return out;
	}

	void FogSolver::CreateResources(ID3D11Device* device, std::uint32_t a_resolution, const NameFn& name)
	{
		ReleaseResources();
		bed = CreateLayer(device, a_resolution, LEVELS, DXGI_FORMAT_R32G32B32A32_FLOAT, name, "Bed");
		depth = CreateLayer(device, a_resolution, LEVELS, DXGI_FORMAT_R32_FLOAT, name, "Depth");
		velocity = CreateLayer(device, a_resolution, LEVELS, DXGI_FORMAT_R16G16_FLOAT, name, "Velocity");
		column = CreateLayer(device, a_resolution, LEVELS, DXGI_FORMAT_R16G16B16A16_FLOAT, name, "Column");
		flux[0] = CreateLayer(device, a_resolution, LEVELS, DXGI_FORMAT_R16G16B16A16_FLOAT, name, "Flux A");
		flux[1] = CreateLayer(device, a_resolution, LEVELS, DXGI_FORMAT_R16G16B16A16_FLOAT, name, "Flux B");
		force = CreateLayer(device, a_resolution, LEVELS * 3, DXGI_FORMAT_R32_SINT, name, "Force");
		contactLow = CreateLayer(device, CONTACT_RESOLUTION, 1, DXGI_FORMAT_R32_UINT, name, "Contact Low");
		contactHigh = CreateLayer(device, CONTACT_RESOLUTION, 1, DXGI_FORMAT_R32_UINT, name, "Contact High");
		pushers = CreateMoverBuffer(device, name, "Pushers");
		contacts = CreateMoverBuffer(device, name, "Contacts");
		const D3D11_BUFFER_DESC desc{ .ByteWidth = sizeof(Constants), .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE, .MiscFlags = 0, .StructureByteStride = 0 };
		winrt::check_hresult(device->CreateBuffer(&desc, nullptr, constants.put()));
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		winrt::check_hresult(device->CreateSamplerState(&samplerDesc, sampler.put()));
		resolution = a_resolution;
		seeded = false;
	}

	bool FogSolver::CompileShaders(const CompileFn& compile)
	{
		bedCS.attach(compile(L"FogBedCS.hlsl"));
		splatCS.attach(compile(L"FogSplatCS.hlsl"));
		inputsCS.attach(compile(L"FogInputsCS.hlsl"));
		fluxCS.attach(compile(L"FogFluxCS.hlsl"));
		depthCS.attach(compile(L"FogDepthCS.hlsl"));
		contactCS.attach(compile(L"FogContactCS.hlsl"));
		if (HasShaders())
			return true;
		ReleaseShaders();
		return false;
	}

	void FogSolver::ReleaseShaders()
	{
		bedCS = splatCS = inputsCS = fluxCS = depthCS = contactCS = nullptr;
	}

	void FogSolver::ReleaseResources()
	{
		bed = depth = velocity = column = force = contactLow = contactHigh = flux[0] = flux[1] = {};
		pushers = contacts = {};
		constants = nullptr;
		sampler = nullptr;
		resolution = 0;
	}

	void FogSolver::BeginStep(ID3D11DeviceContext* context, const StepParams& grid, const FogLayerParams& layer, const FogGround& ground, const FogMovers& movers)
	{
		if (!seeded) {
			// Forces start at zero; afterwards each step's inputs clear what they read
			const UINT zero[4] = {};
			context->ClearUnorderedAccessViewUint(force.uav.get(), zero);
		}
		const auto upload = [&](Buffer& a_buffer, std::span<const Mover> a_movers) {
			const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(a_movers.size(), MAX_MOVERS));
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (count > 0 && SUCCEEDED(context->Map(a_buffer.buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				std::memcpy(mapped.pData, a_movers.data(), count * sizeof(Mover));
				context->Unmap(a_buffer.buffer.get(), 0);
				return count;
			}
			return 0u;
		};
		pusherCount = upload(pushers, movers.pushers);
		contactCount = upload(contacts, movers.contacts);

		Constants c{};
		c.gridMin[0] = grid.gridMin[0];
		c.gridMin[1] = grid.gridMin[1];
		// Unseeded, every cell is new: the previous window lies far away
		c.prevGridMin[0] = seeded ? prevGridMin[0] : INT_MIN / 2;
		c.prevGridMin[1] = seeded ? prevGridMin[1] : INT_MIN / 2;
		c.cellSize = grid.cellSize;
		c.deltaTime = grid.deltaTime;
		c.resolution = resolution;
		c.gravity = layer.gravity;
		c.groundThickness = layer.groundThickness;
		c.poolDepth = layer.poolDepth;
		c.sourceRate = layer.sourceRate;
		c.evaporateRate = layer.evaporateRate;
		c.fluxDamping = layer.fluxDamping;
		c.confinement = layer.confinement;
		c.hollowGain = layer.hollowGain;
		c.windX = layer.wind[0];
		c.windY = layer.wind[1];
		c.drift = layer.drift;
		c.time = layer.time;
		c.rain = layer.rain;
		std::memcpy(c.occlusionViewProj, layer.occlusionViewProj, sizeof(c.occlusionViewProj));
		std::memcpy(c.eye, layer.eye, sizeof(c.eye));
		c.hasOcclusion = layer.occlusion ? 1 : 0;
		std::memcpy(c.slots, ground.slots.data(), sizeof(c.slots));
		c.tileMin[0] = ground.tileMin[0];
		c.tileMin[1] = ground.tileMin[1];
		c.landCell[0] = ground.landCell[0];
		c.landCell[1] = ground.landCell[1];
		c.hasLand = ground.land ? 1 : 0;
		c.moverCount = pusherCount;
		c.contactCount = contactCount;
		c.contactCell = CONTACT_CELL;
		c.contactMin[0] = movers.contactMin[0];
		c.contactMin[1] = movers.contactMin[1];
		c.contactResolution = CONTACT_RESOLUTION;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(context->Map(constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			std::memcpy(mapped.pData, &c, sizeof(c));
			context->Unmap(constants.get(), 0);
		}
		ID3D11Buffer* cb = constants.get();
		context->CSSetConstantBuffers(0, 1, &cb);
		prevGridMin[0] = grid.gridMin[0];
		prevGridMin[1] = grid.gridMin[1];
		seeded = true;
	}

	void FogSolver::Dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, std::span<ID3D11ShaderResourceView* const> srvs, std::span<ID3D11UnorderedAccessView* const> uavs, std::uint32_t a_groupsX, std::uint32_t a_slices)
	{
		context->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), uavs.data(), nullptr);
		context->CSSetShader(shader, nullptr, 0);
		const UINT groups = (resolution + 7) / 8;
		if (a_groupsX == 0)
			context->Dispatch(groups, groups, a_slices);
		else
			context->Dispatch(a_groupsX, 1, 1);
		// Unbound, so the next pass can read what this one wrote
		ID3D11ShaderResourceView* nullSRVs[4] = {};
		ID3D11UnorderedAccessView* nullUAVs[4] = {};
		context->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), nullSRVs);
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(uavs.size()), nullUAVs, nullptr);
	}

	void FogSolver::Bed(ID3D11DeviceContext* context, const FogGround& ground)
	{
		ID3D11SamplerState* s = sampler.get();
		context->CSSetSamplers(0, 1, &s);
		ID3D11ShaderResourceView* srvs[] = { ground.tiles, ground.land };
		ID3D11UnorderedAccessView* uavs[] = { bed.uav.get() };
		Dispatch(context, bedCS.get(), srvs, uavs, 0, 1);
	}

	void FogSolver::Splat(ID3D11DeviceContext* context)
	{
		if (pusherCount == 0)
			return;
		ID3D11ShaderResourceView* srvs[] = { pushers.srv.get(), bed.srv.get() };
		ID3D11UnorderedAccessView* uavs[] = { force.uav.get() };
		Dispatch(context, splatCS.get(), srvs, uavs, pusherCount, 1);
	}

	void FogSolver::Inputs(ID3D11DeviceContext* context, const FogLayerParams& layer)
	{
		ID3D11ShaderResourceView* srvs[] = { bed.srv.get(), velocity.srv.get(), flux[0].srv.get(), layer.occlusion };
		ID3D11UnorderedAccessView* uavs[] = { depth.uav.get(), flux[1].uav.get(), force.uav.get() };
		Dispatch(context, inputsCS.get(), srvs, uavs, 0, LEVELS);
	}

	void FogSolver::Flux(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { bed.srv.get(), depth.srv.get(), flux[1].srv.get() };
		ID3D11UnorderedAccessView* uavs[] = { flux[0].uav.get() };
		Dispatch(context, fluxCS.get(), srvs, uavs, 0, LEVELS);
	}

	void FogSolver::Depth(ID3D11DeviceContext* context)
	{
		ID3D11ShaderResourceView* srvs[] = { flux[0].srv.get(), bed.srv.get() };
		ID3D11UnorderedAccessView* uavs[] = { depth.uav.get(), velocity.uav.get(), column.uav.get() };
		Dispatch(context, depthCS.get(), srvs, uavs, 0, LEVELS);
	}

	void FogSolver::Contact(ID3D11DeviceContext* context)
	{
		const UINT highest[4] = { UINT_MAX, UINT_MAX, UINT_MAX, UINT_MAX }, zero[4] = {};
		context->ClearUnorderedAccessViewUint(contactLow.uav.get(), highest);
		context->ClearUnorderedAccessViewUint(contactHigh.uav.get(), zero);
		if (contactCount == 0)
			return;
		ID3D11ShaderResourceView* srvs[] = { contacts.srv.get() };
		ID3D11UnorderedAccessView* uavs[] = { contactLow.uav.get(), contactHigh.uav.get() };
		Dispatch(context, contactCS.get(), srvs, uavs, contactCount, 1);
	}

	void FogSolver::EndStep(ID3D11DeviceContext* context)
	{
		ID3D11Buffer* nullCB = nullptr;
		ID3D11SamplerState* nullSampler = nullptr;
		context->CSSetConstantBuffers(0, 1, &nullCB);
		context->CSSetSamplers(0, 1, &nullSampler);
		context->CSSetShader(nullptr, nullptr, 0);
	}
}
