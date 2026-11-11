#pragma once

// Free of plugin headers (no PCH, globals or CommonLib) so tests/FluidField can run the solver on a WARP device.

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace FluidFieldSim
{
	/**
	 * @brief D193: something moving through the air, matching `Mover` in SplatCS.hlsl: a segment (a point when both ends
	 * match) with a Gaussian falloff, drawn into every level it reaches. World units throughout.
	 */
	struct Mover
	{
		float from[2];      ///< Segment start, world xy
		float to[2];        ///< Segment end, world xy
		float radius;       ///< Gaussian falloff radius
		float shove;        ///< World units per second it shoves the fog aside at its edge
		float velocity[2];  ///< Wind impulse, world units per second, added once
		float carve;        ///< Share of the fog it clears outright where it passes (0 to 1)
		float pad[3];
	};
	static_assert(sizeof(Mover) == 48);

	/** @brief Inputs for the fog layer (D21), matching FogCB in FogCS.hlsl. */
	struct FogParams
	{
		float drift[2]{};          ///< Weather wind carrying the fog, world units per second
		float refillRate = 0.25f;  ///< Per-second rate at which displaced fog settles back
		float stirRate = 0.5f;     ///< Per-second rate at which fully stirred air thins the fog
		float stirSpeed = 120.0f;  ///< Wind speed counted as fully stirred, world units per second
		float pad0 = 0.0f;
		float maxFog = 2.0f;  ///< Most fog a cell can be packed to, as a multiple of the resting amount
		float pad = 0.0f;
		float patternOffset[2]{};  ///< How far the resting pattern has drifted with the weather's wind, world units
		float pad2[2]{};
	};
	static_assert(sizeof(FogParams) == 48);

	class WindSolver;

	/** @brief Inputs for one simulation step. */
	struct StepParams
	{
		std::int32_t gridMin[2]{};   ///< World cell of logical texel (0, 0)
		float cellSize = 16.0f;      ///< World units per texel
		float deltaTime = 0.0f;      ///< Seconds to simulate
		float velocityDecay = 1.0f;  ///< Multiplier applied after advection
		std::uint32_t jacobiIterations = 30;
		std::span<const Mover> movers;
		const FogParams* fog = nullptr;  ///< Steps the fog layer too when set
		/** D193: the coarser level around this one: its edges and new cells come from it. */
		const WindSolver* parent = nullptr;
		/** D193: the finer level inside this one, when it runs this frame: movers inside it are its own (it is absorbed). */
		const WindSolver* child = nullptr;
	};

	/**
	 * @brief 2D stable-fluids wind layer on a toroidal, scrolling grid. Owns its GPU resources.
	 * Each step dispatches splat, add forces, advect, divergence, a Jacobi pressure solve and projection. D193: one per
	 * level; a level takes its edges from its parent and absorbs its child (Absorb).
	 */
	class WindSolver
	{
	public:
		static constexpr std::uint32_t MAX_MOVERS = 512;
		/** @brief Cells along a level's edge that relax toward its parent, and a child's that its parent does not absorb. */
		static constexpr std::uint32_t EDGE_BAND = 6;

		/** @brief Names a D3D11 object for RenderDoc. */
		using NameFn = std::function<void(ID3D11DeviceChild*, const char*)>;
		/** @brief Compiles a compute shader from a file under the FluidField shader folder; returns an owned reference or null. */
		using CompileFn = std::function<ID3D11ComputeShader*(const wchar_t* fileName)>;

		/** @brief Creates the layer textures and buffers. Releases any previous ones first. */
		void CreateResources(ID3D11Device* device, std::uint32_t resolution, const NameFn& name, const char* label = "");
		/** @brief Compiles every pass. Returns false if any pass failed, in which case the solver stays inactive. */
		bool CompileShaders(const CompileFn& compile);
		void ReleaseShaders();
		void ReleaseResources();

		bool HasResources() const { return resolution != 0; }
		bool HasShaders() const;
		std::uint32_t GetResolution() const { return resolution; }

		/** @brief Clears every layer; the next step starts from its parent (or rest) whatever the scroll. */
		void Reset(ID3D11DeviceContext* context);

		/**
		 * @brief Runs one simulation step.
		 * @param wrap Called as `wrap(passName, runPass)` around each pass, for profiling zones.
		 */
		template <class Wrap>
		void Step(ID3D11DeviceContext* context, const StepParams& params, Wrap&& wrap)
		{
			BeginStep(context, params);
			wrap("Splat", [&] { Splat(context); });
			wrap("Add Forces", [&] { AddForces(context, params.parent); });
			wrap("Advect", [&] { Advect(context); });
			wrap("Divergence", [&] { Divergence(context); });
			wrap("Pressure Solve", [&] { SolvePressure(context, params.jacobiIterations); });
			wrap("Project", [&] { Project(context); });
			if (params.fog)
				wrap("Fog", [&] { Fog(context, params.parent); });
			EndStep(context);
		}

		/** @brief Runs one step with no profiling. */
		void Step(ID3D11DeviceContext* context, const StepParams& params)
		{
			Step(context, params, [](const char*, auto&& runPass) { runPass(); });
		}

		/** @brief D193: takes the finer level's wind and fog where it covers this one (after both have stepped). */
		void Absorb(ID3D11DeviceContext* context, const WindSolver& child);

		ID3D11Texture2D* GetVelocityTexture() const { return velocity[current].texture.get(); }
		ID3D11Texture2D* GetDivergenceTexture() const { return divergence.texture.get(); }
		ID3D11Texture2D* GetPressureTexture() const { return pressure[pressureResult].texture.get(); }

		ID3D11ShaderResourceView* GetVelocitySRV() const { return velocity[current].srv.get(); }
		/** @brief Fog density multiplier (D21): 1 is the weather's fog, 0 clear air. Toroidal like the velocity. */
		ID3D11ShaderResourceView* GetFogSRV() const { return fog[currentFog].srv.get(); }

		/** @brief Where the last step placed the grid (world cell of logical texel 0, 0) and its cell size. */
		const std::int32_t* GetGridMin() const { return prevGridMin; }
		float GetCellSize() const { return cellSize; }

	private:
		struct Layer
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		};

		// Matches FluidFieldCB in Common.hlsli
		struct alignas(16) Constants
		{
			std::int32_t gridMin[2];
			std::int32_t prevGridMin[2];
			std::uint32_t gridRes;
			float cellSize;
			float deltaTime;
			float velocityDecay;
			std::uint32_t historyValid;
			std::uint32_t emitterCount;
			std::uint32_t pad[2];
		};
		static_assert(sizeof(Constants) == 48);

		// Matches LevelCB in Levels.hlsli: the level's parent (or the child it absorbs) and its movers
		struct alignas(16) LevelConstants
		{
			std::int32_t otherGridMin[2];
			std::uint32_t otherGridRes;
			float otherCellSize;
			std::int32_t childGridMin[2];
			std::uint32_t childGridRes;
			float childCellSize;
			std::uint32_t hasOther;
			std::uint32_t hasChild;
			std::uint32_t moverCount;
			std::uint32_t edgeBand;
		};
		static_assert(sizeof(LevelConstants) == 48);

		static Layer CreateLayer(ID3D11Device* device, std::uint32_t resolution, DXGI_FORMAT format, const NameFn& name, const std::string& label);

		void BeginStep(ID3D11DeviceContext* context, const StepParams& params);
		void EndStep(ID3D11DeviceContext* context);
		void WriteLevel(ID3D11DeviceContext* context, const WindSolver* other, const WindSolver* child, std::uint32_t moverCount);
		void Splat(ID3D11DeviceContext* context);
		void AddForces(ID3D11DeviceContext* context, const WindSolver* parent);
		void Advect(ID3D11DeviceContext* context);
		void Divergence(ID3D11DeviceContext* context);
		void SolvePressure(ID3D11DeviceContext* context, std::uint32_t iterations);
		void Project(ID3D11DeviceContext* context);
		void Fog(ID3D11DeviceContext* context, const WindSolver* parent);

		/** @brief Binds SRVs and UAVs, then dispatches over the whole grid. */
		void Dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, std::span<ID3D11ShaderResourceView* const> srvs, std::span<ID3D11UnorderedAccessView* const> uavs);
		void Dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, std::span<ID3D11ShaderResourceView* const> srvs, ID3D11UnorderedAccessView* uav)
		{
			Dispatch(context, shader, srvs, std::span<ID3D11UnorderedAccessView* const>(&uav, 1));
		}

		std::uint32_t resolution = 0;
		float cellSize = 0.0f;
		std::uint32_t moverCount = 0;
		bool stepFog = false;

		std::array<Layer, 2> velocity;  // Ping-pong, `current` holds the latest result
		std::array<Layer, 2> pressure;  // Ping-pong within the Jacobi solve
		Layer divergence;
		std::array<Layer, 2> fog;  // Ping-pong, `currentFog` holds the latest result
		/** D193: the movers drawn in this step, fixed point (Levels.hlsli): wind x, y, fog shove x, y, carve. */
		std::array<Layer, 5> splat;
		std::uint32_t currentFog = 0;
		std::uint32_t current = 0;
		std::uint32_t pressureResult = 0;

		winrt::com_ptr<ID3D11Buffer> constants;
		winrt::com_ptr<ID3D11Buffer> fogConstants;
		winrt::com_ptr<ID3D11Buffer> levelConstants;
		winrt::com_ptr<ID3D11Buffer> moverBuffer;
		winrt::com_ptr<ID3D11ShaderResourceView> moverSRV;

		winrt::com_ptr<ID3D11ComputeShader> splatCS;
		winrt::com_ptr<ID3D11ComputeShader> addForcesCS;
		winrt::com_ptr<ID3D11ComputeShader> advectCS;
		winrt::com_ptr<ID3D11ComputeShader> divergenceCS;
		winrt::com_ptr<ID3D11ComputeShader> jacobiCS;
		winrt::com_ptr<ID3D11ComputeShader> projectCS;
		winrt::com_ptr<ID3D11ComputeShader> fogCS;
		winrt::com_ptr<ID3D11ComputeShader> absorbCS;

		// Scroll and reset state carried between steps
		std::int32_t prevGridMin[2]{};
		bool historyValid = false;
	};
}
