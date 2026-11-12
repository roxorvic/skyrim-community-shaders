#pragma once

// Free of plugin headers (no PCH, globals or CommonLib) so tests/FluidField can run the solver on a WARP device.

#include "SolverCommon.h"

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <span>

namespace FluidFieldSim
{
	/** @brief The fog's ground: GroundTiles' resident texture and land map, as FogCB takes them. */
	struct FogGround
	{
		ID3D11ShaderResourceView* tiles = nullptr;  ///< Top height, flags, lower height, flags per 8-unit texel, toroidal
		ID3D11ShaderResourceView* land = nullptr;   ///< GroundTiles::GetLandSRV, where no tile is resident; may be null
		std::int32_t tileMin[2]{};
		std::array<std::int32_t, 256 * 4> slots{};  ///< Per resident slot (16 a side): tile x, y, valid, pad
		std::int32_t landCell[2]{};                 ///< GroundTiles::GetLandCell
	};

	/** @brief The layer's look and behaviour for a step. */
	struct FogLayerParams
	{
		float gravity = 150.0f;  ///< Reduced gravity, world units per second squared: how fast fog runs downhill
		float groundThickness = 20.0f;
		float poolDepth = 40.0f;
		float sourceRate = 0.1f;     ///< Per second, toward Ground Thickness
		float evaporateRate = 3.0f;  ///< Per second, of depth over Pool Depth
		float fluxDamping = 3.0f;    ///< Per second
		float confinement = 2.0f;
		float hollowGain = 0.5f;  ///< Settled depth per unit of hollowness
		float wind[2]{};          ///< The weather's wind carrying the fog, world units per second
		float drift = 25.0f;      ///< Ambient drift (slow eddies the fog wanders in), world units per second
		float time = 0.0f;        ///< Seconds, for the ambient drift
		float rain = 0.0f;        ///< How hard it rains, 0 to 1
		/** @brief The precipitation occlusion map (rain only where the sky is open), its projection from the eye; none when null. */
		ID3D11ShaderResourceView* occlusion = nullptr;
		float occlusionViewProj[16]{};
		float eye[3]{};
	};

	/** @brief What moves through the fog this step: pushers for the sim, capsules for the contact map round contactMin. */
	struct FogMovers
	{
		std::span<const Mover> pushers;
		std::span<const Mover> contacts;
		std::int32_t contactMin[2]{};  ///< World texel (of CONTACT_CELL) of the contact map's texel (0, 0)
	};

	/**
	 * @brief D194: heavy fog on a toroidal grid round the player, moved by a virtual-pipe model (FogCommon.hlsli); D195: up
	 * to two levels a cell. Each step: beds from the ground, movers splatted, inputs (seeding, sources, evaporation, pushes,
	 * cuts, confinement), flux, depth, and the contact map. Owns its GPU resources.
	 */
	class FogSolver
	{
	public:
		static constexpr std::uint32_t LEVELS = 2;
		static constexpr std::uint32_t MAX_MOVERS = 256;
		static constexpr std::uint32_t CONTACT_RESOLUTION = 512;
		static constexpr float CONTACT_CELL = 6.0f;

		void CreateResources(ID3D11Device* device, std::uint32_t resolution, const NameFn& name);
		bool CompileShaders(const CompileFn& compile);
		void ReleaseShaders();
		void ReleaseResources();
		bool HasResources() const { return resolution != 0; }
		bool HasShaders() const { return bedCS && splatCS && inputsCS && fluxCS && depthCS && contactCS; }
		std::uint32_t GetResolution() const { return resolution; }

		/** @brief The next step seeds every cell with the settled field. */
		void Reset() { seeded = false; }

		/**
		 * @brief Runs one step.
		 * @param wrap Called as `wrap(passName, runPass)` around each pass, for profiling zones.
		 */
		template <class Wrap>
		void Step(ID3D11DeviceContext* context, const StepParams& grid, const FogLayerParams& layer, const FogGround& ground, const FogMovers& movers, Wrap&& wrap)
		{
			BeginStep(context, grid, layer, ground, movers);
			wrap("Fog Bed", [&] { Bed(context, ground); });
			wrap("Fog Splat", [&] { Splat(context); });
			wrap("Fog Inputs", [&] { Inputs(context, layer); });
			wrap("Fog Flux", [&] { Flux(context); });
			wrap("Fog Depth", [&] { Depth(context); });
			wrap("Fog Contact", [&] { Contact(context); });
			EndStep(context);
		}
		void Step(ID3D11DeviceContext* context, const StepParams& grid, const FogLayerParams& layer, const FogGround& ground, const FogMovers& movers = {})
		{
			Step(context, grid, layer, ground, movers, [](const char*, auto&& runPass) { runPass(); });
		}

		/** @brief Per level (array slice): depth, velocity (xy) and churn per cell, toroidal (world cell c in texel c & (resolution - 1)). */
		ID3D11ShaderResourceView* GetColumnSRV() const { return column.srv.get(); }
		/** @brief Per level, as GetColumnSRV: x height, y fog source share, z kind (FogCommon.hlsli). */
		ID3D11ShaderResourceView* GetBedSRV() const { return bed.srv.get(); }
		/** @brief The contact map's lowest and highest points per texel (encoded as FogContactCS's EncodeHeight). */
		ID3D11ShaderResourceView* GetContactLowSRV() const { return contactLow.srv.get(); }
		ID3D11ShaderResourceView* GetContactHighSRV() const { return contactHigh.srv.get(); }
		ID3D11Texture2D* GetDepthTexture() const { return depth.texture.get(); }
		ID3D11Texture2D* GetBedTexture() const { return bed.texture.get(); }  ///< For tests/FluidField

	private:
		struct Layer
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		};
		struct Buffer
		{
			winrt::com_ptr<ID3D11Buffer> buffer;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
		};
		/** @brief Matches FogCB in FogCommon.hlsli. */
		struct alignas(16) Constants
		{
			std::int32_t gridMin[2];
			std::int32_t prevGridMin[2];
			float cellSize, deltaTime;
			std::uint32_t resolution;
			float gravity, groundThickness, poolDepth, sourceRate, evaporateRate, fluxDamping, confinement, hollowGain, pad0;
			std::int32_t slots[256 * 4];
			std::int32_t tileMin[2];
			std::int32_t landCell[2];
			std::uint32_t hasLand, moverCount, contactCount;
			float contactCell;
			std::int32_t contactMin[2];
			std::uint32_t contactResolution;
			float windX, windY, drift, time;
			float rain;
			float occlusionViewProj[16];
			float eye[3];
			std::uint32_t hasOcclusion;
		};
		static_assert(sizeof(Constants) == 1232 + 192 * 16);

		static Layer CreateLayer(ID3D11Device* device, std::uint32_t resolution, std::uint32_t slices, DXGI_FORMAT format, const NameFn& name, const char* label);
		static Buffer CreateMoverBuffer(ID3D11Device* device, const NameFn& name, const char* label);
		void BeginStep(ID3D11DeviceContext* context, const StepParams& grid, const FogLayerParams& layer, const FogGround& ground, const FogMovers& movers);
		void Bed(ID3D11DeviceContext* context, const FogGround& ground);
		void Splat(ID3D11DeviceContext* context);
		void Inputs(ID3D11DeviceContext* context, const FogLayerParams& layer);
		void Flux(ID3D11DeviceContext* context);
		void Depth(ID3D11DeviceContext* context);
		void Contact(ID3D11DeviceContext* context);
		void EndStep(ID3D11DeviceContext* context);
		/** @brief Binds, dispatches and unbinds; a_groupsX 0 covers the grid (a_slices groups deep), else that many groups. */
		void Dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, std::span<ID3D11ShaderResourceView* const> srvs, std::span<ID3D11UnorderedAccessView* const> uavs, std::uint32_t a_groupsX, std::uint32_t a_slices);

		std::uint32_t resolution = 0;
		Layer bed, depth, velocity, column, force, contactLow, contactHigh;
		Layer flux[2];  ///< Ping-pong; flux[0] holds the step's result
		Buffer pushers, contacts;
		std::uint32_t pusherCount = 0, contactCount = 0;
		winrt::com_ptr<ID3D11Buffer> constants;
		winrt::com_ptr<ID3D11SamplerState> sampler;
		winrt::com_ptr<ID3D11ComputeShader> bedCS, splatCS, inputsCS, fluxCS, depthCS, contactCS;
		std::int32_t prevGridMin[2]{};
		bool seeded = false;
	};
}
