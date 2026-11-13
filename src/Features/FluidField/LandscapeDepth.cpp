#include "LandscapeDepth.h"

#include "Globals.h"
#include "Profiler.h"
#include "Utils/Game.h"
#include "Utils/VersionedRelocation.h"

#include <winrt/base.h>

namespace FluidFieldSim
{
	namespace
	{
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11DepthStencilView> dsv;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		bool enabled = false;
		bool inPrepass = false;
		bool drawn = false;  ///< Something was drawn into it this frame
		float reach = 0.0f, frameReach = 0.0f;
		float nearSkip = 0.0f;  ///< Landscape passes wholly within this of the eye are not drawn again (the tiles cover it)
		RE::NiPoint3 eye{};

		/** @brief The target, sized as the main depth; false if it could not be made. */
		bool EnsureTarget()
		{
			auto& mainDepth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			if (!mainDepth.texture)
				return false;
			D3D11_TEXTURE2D_DESC main{};
			mainDepth.texture->GetDesc(&main);
			if (texture) {
				D3D11_TEXTURE2D_DESC current{};
				texture->GetDesc(&current);
				if (current.Width == main.Width && current.Height == main.Height)
					return true;
			}
			texture = nullptr;
			dsv = nullptr;
			srv = nullptr;
			const D3D11_TEXTURE2D_DESC desc{ .Width = main.Width, .Height = main.Height, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R32_TYPELESS, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = 0 };
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
			dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			auto* device = globals::d3d::device;
			if (FAILED(device->CreateTexture2D(&desc, nullptr, texture.put())) || FAILED(device->CreateDepthStencilView(texture.get(), &dsvDesc, dsv.put())) ||
				FAILED(device->CreateShaderResourceView(texture.get(), &srvDesc, srv.put()))) {
				texture = nullptr;
				dsv = nullptr;
				srv = nullptr;
				return false;
			}
			Util::SetResourceName(texture.get(), "FluidField::LandscapeDepth");
			return true;
		}

		struct Main_RenderDepth
		{
			static void thunk(bool a1, bool a2)
			{
				if (!enabled || !EnsureTarget()) {
					drawn = false;
					func(a1, a2);
					return;
				}
				globals::d3d::context->ClearDepthStencilView(dsv.get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
				eye = Util::GetEyePosition();
				frameReach = 0.0f;
				drawn = false;
				inPrepass = true;
				func(a1, a2);
				inPrepass = false;
				if (drawn)
					reach = frameReach;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSBatchRenderer_RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
			{
				func(a_pass, a_technique, a_alphaTest, a_renderFlags);
				if (!inPrepass || !a_pass || !a_pass->shaderProperty || !a_pass->geometry)
					return;
				using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
				if (!a_pass->shaderProperty->flags.any(Flag::kLODLandscape, Flag::kMultiTextureLandscape))
					return;
				auto* triShape = a_pass->geometry->AsTriShape();
				if (!triShape)
					return;
				// The distant fog only reads it past nearSkip: loaded cells wholly inside are left out
				const auto& bound = a_pass->geometry->worldBound;
				const float extent = std::hypot(bound.center.x - eye.x, bound.center.y - eye.y) + bound.radius;
				if (extent < nearSkip)
					return;
				globals::profiler->BeginPass("FluidField::Landscape Depth");
				// The same draw again, the game's state still bound, into the landscape's own depth
				auto* context = globals::d3d::context;
				ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
				ID3D11DepthStencilView* oldDsv = nullptr;
				context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &oldDsv);
				ID3D11PixelShader* pixelShader = nullptr;
				ID3D11ClassInstance* instances[256]{};
				UINT instanceCount = 256;
				context->PSGetShader(&pixelShader, instances, &instanceCount);
				context->OMSetRenderTargets(0, nullptr, dsv.get());
				context->PSSetShader(nullptr, nullptr, 0);
				context->DrawIndexed(triShape->GetTrishapeRuntimeData().triangleCount * 3, 0, 0);
				context->PSSetShader(pixelShader, instances, instanceCount);
				context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, oldDsv);
				for (auto* rtv : rtvs)
					if (rtv)
						rtv->Release();
				if (oldDsv)
					oldDsv->Release();
				if (pixelShader)
					pixelShader->Release();
				for (UINT i = 0; i < instanceCount; i++)
					if (instances[i])
						instances[i]->Release();
				globals::profiler->EndPass();
				drawn = true;
				frameReach = std::max(frameReach, extent);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void LandscapeDepth::Install()
	{
		static bool installed = false;
		if (installed)
			return;
		installed = true;
		// The same call sites as Terrain Blending's hooks: installed after them, these wrap theirs
		stl::write_thunk_call<Main_RenderDepth>(REL::RelocationID(35560, 36559).address() + Util::VersionedRelocation::Select(0x395, 0x395, 0x3B3));
		stl::write_thunk_call<BSBatchRenderer_RenderPassImmediately>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));
		logger::info("[Fluid Field] Ground fog: landscape depth hooks installed");
	}

	void LandscapeDepth::SetEnabled(bool a_enabled)
	{
		enabled = a_enabled;
		if (!enabled) {
			texture = nullptr;
			dsv = nullptr;
			srv = nullptr;
			drawn = false;
		}
	}

	void LandscapeDepth::SetNearSkip(float a_distance)
	{
		nearSkip = a_distance;
	}

	ID3D11ShaderResourceView* LandscapeDepth::GetSRV()
	{
		return drawn ? srv.get() : nullptr;
	}

	float LandscapeDepth::GetReach()
	{
		return reach;
	}
}
