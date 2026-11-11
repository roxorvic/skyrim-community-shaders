#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "FluidField/AirSources.h"
#include "FluidField/GroundHeightMap.h"
#include "FluidField/WindSolver.h"

/**
 * @brief Ground Fog: fog lying on the ground around the player, carried by a stable-fluids wind layer.
 *
 * The wind runs on toroidal grids that follow the player (D193): a fine level close by (only while something moves in it),
 * a medium one and a far one round it, each taking its edges from the next and absorbing the finer one. Moving actors push
 * the air and part the fog, and the weather's wind drifts it. Drawn over the finished world. Runs only while this feature
 * is on.
 */
struct FluidFieldGroundFog : Feature
{
	virtual inline std::string GetName() override { return "Ground Fog"; }
	virtual std::string GetDisplayName() override { return T("feature.fluid_field_ground_fog.name", "Ground Fog"); }
	virtual inline std::string GetShortName() override { return "FluidFieldGroundFog"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kFluidField; }

	/** @brief The UI summary panel's description and key features. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.fluid_field_ground_fog.description", "Low fog lying on the ground around the player, in banks and clearings."),
			{ T("feature.fluid_field_ground_fog.key_feature_1", "Parts and swirls around moving actors, then settles back"),
				T("feature.fluid_field_ground_fog.key_feature_2", "Drifts with the weather's wind") } };
	}

	static constexpr uint MIN_JACOBI_ITERATIONS = 1;
	static constexpr uint MAX_JACOBI_ITERATIONS = 100;
	/** @brief D193: the levels, fine to far; each is LEVEL_RESOLUTION cells per side. */
	static constexpr uint LEVEL_COUNT = 3;
	static constexpr uint LEVEL_RESOLUTION = 128;
	/** @brief The medium and far levels' cell sizes, world units (the fine level's follows Fine Radius). */
	static constexpr float MEDIUM_CELL = 32.0f;
	static constexpr float FAR_CELL = 96.0f;
	static constexpr float MIN_FINE_RADIUS = 384.0f;
	static constexpr float MAX_FINE_RADIUS = 1536.0f;

	struct Settings
	{
		bool Enabled = false;
		float FogDensity = 0.08f;        ///< Ground fog extinction per world unit inside the layer, in its banks
		float FogBrightness = 0.15f;     ///< How brightly the ground fog is lit, as a fraction of the light around it
		float FogThickness = 18.0f;      ///< Height of the ground fog layer's top above the ground, in its banks, world units
		float FogStir = 1.0f;            ///< How strongly moving actors push the air and part the fog
		float FogRefillSeconds = 10.0f;  ///< Seconds for parted fog to fill back in (about two thirds of the way)
		uint SimLevels = 3;              ///< D193: 1 medium only; 2 fine and medium; 3 fine, medium and far
		float FineRadius = 768.0f;       ///< D193: the fine level's reach round the player, world units (its cell size follows)
		uint FarUpdateRate = 2;          ///< D193: the far level steps every this many frames
		uint JacobiIterations = 42;      ///< Pressure solve iterations per step (the fine level's; coarser levels use fewer)
		float WindDamping = 0.2f;        ///< Fraction of wind speed lost per second, as an exponential rate
	};

	Settings settings;

	/** @brief Draws the ImGui settings UI. */
	virtual void DrawSettings() override;

	/** @brief Loads and sanitizes the settings. */
	virtual void LoadSettings(json& o_json) override;
	/** @brief Saves the settings. */
	virtual void SaveSettings(json& o_json) override;
	/** @brief Restores the default settings. */
	virtual void RestoreDefaultSettings() override;

	/** @brief Registers the shared loading-screen listener. */
	virtual void PostPostLoad() override;

	/** @brief Steps the wind and fog. */
	virtual void Prepass() override;

	/** @brief Drops the wind and fog shaders so they recompile. */
	virtual void ClearShaderCache() override;

	/** @brief Clamps loaded settings to the ranges the solver supports. */
	static Settings Sanitize(Settings a_settings);

	/** @brief Loaded and enabled. */
	bool IsActive() const { return loaded && settings.Enabled; }

	/** @brief D21: draws the fog over the finished world, before first person, TAA and tonemapping. */
	void DrawAfterWorld();

private:
	/** @brief Creates GPU resources and shaders as needed; false if the solver cannot run. */
	bool EnsureSolver();
	/** @brief Releases the wind, the fog's ground map and its pass. */
	void Release();
	bool released = true;
	/** @brief Loading screens this feature has reset for (Field::ConsumeLoad). */
	uint32_t seenLoads = UINT32_MAX;

	/** @brief World units per texel of a level. */
	float GetCellSize(uint a_level) const { return a_level == 0 ? 2.0f * settings.FineRadius / static_cast<float>(LEVEL_RESOLUTION) : a_level == 1 ? MEDIUM_CELL :
	                                                                                                                                                 FAR_CELL; }
	/** @brief Whether a level is in use at the current Sim Levels. */
	bool UsesLevel(uint a_level) const { return a_level == 1 || (a_level == 0 && settings.SimLevels >= 2) || (a_level == 2 && settings.SimLevels >= 3); }

	void DrawGroundFog(ID3D11Texture2D* a_mainTexture, ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth);

	/** @brief Per level: its solver, where it sits and its share of the drawn fog. */
	struct Level
	{
		FluidFieldSim::WindSolver solver;
		int32_t gridMin[2]{};
		float cellSize = 0.0f;     ///< Cell size it was last stepped with; a change invalidates its contents
		float pendingTime = 0.0f;  ///< Seconds not yet stepped (the far level steps every few frames)
		float weight = 0.0f;       ///< Share of the drawn fog taken from it (the fine level fades in and out)
		bool running = false;      ///< Stepped this frame
	};
	std::array<Level, LEVEL_COUNT> levels;
	/** @brief Seconds since anything moved in the fine level's reach; it idles past FINE_IDLE_SECONDS. */
	float fineIdle = 1e9f;
	uint32_t frameCounter = 0;
	static constexpr float FINE_IDLE_SECONDS = 5.0f;
	static constexpr float FINE_FADE_IN_SECONDS = 0.5f;
	static constexpr float FINE_FADE_OUT_SECONDS = 1.5f;
	/** @brief World units per second a running actor shoves the fog aside, at Stirring 1. */
	static constexpr float SHOVE_SPEED = 150.0f;
	FluidFieldSim::AirSources airSources;
	FluidFieldSim::FogParams fogParams;  ///< This step's fog inputs

	/** @brief Samples along each pixel's ray through the ground fog. */
	static constexpr uint32_t GROUND_FOG_STEPS = 48;
	/** @brief Matches GroundFogCB in GroundFogCS.hlsl. */
	struct alignas(16) GroundFogConstants
	{
		int32_t levelGridMin[LEVEL_COUNT][4];  ///< xy used
		float levelInfo[LEVEL_COUNT][4];       ///< x cell size, y weight, z resolution
		float density;
		float thickness;
		float maxHeight;
		uint32_t stepCount;
		float albedo[3];
		float maxDistance;
		int32_t groundMin[2];
		uint32_t groundRes;
		float groundCellSize;
		float patternOffset[2];
		uint32_t debugMode;
		float pad;
		int32_t fineGroundMin[2];
		uint32_t fineGroundRes;
		float fineGroundCellSize;
	};
	static_assert(sizeof(GroundFogConstants) == 176);
	/** @brief The fog's ground map: texels per side and their size (as wide as the far level). */
	static constexpr uint32_t FOG_GROUND_RESOLUTION = 512;
	static constexpr float FOG_GROUND_CELL = 24.0f;
	/** @brief D193: a finer ground map near the player (as wide as the medium level): ledges' and rocks' edges, the walls. */
	static constexpr uint32_t FOG_FINE_GROUND_RESOLUTION = 512;
	static constexpr float FOG_FINE_GROUND_CELL = 8.0f;
	/** @brief Fog drift at the weather's full wind speed, world units per second. */
	static constexpr float MAX_WEATHER_DRIFT = 150.0f;

	FluidFieldSim::GroundMeshes groundMeshes;
	FluidFieldSim::GroundHeightMap groundHeight;
	FluidFieldSim::GroundHeightMap fineGround;
	bool fineGroundReady = false;
	int32_t fogFineGroundMin[2]{};
	winrt::com_ptr<ID3D11ComputeShader> groundFogCS;
	winrt::com_ptr<ID3D11ComputeShader> groundFogCompositeCS;  ///< D193: lays the half-resolution fog over the scene
	std::unique_ptr<Texture2D> fogHalf;                        ///< In-scattered light (rgb) and transmittance (a), half resolution
	std::unique_ptr<Texture2D> fogHalfDistance;                ///< Distance to each half-resolution texel's surface
	winrt::com_ptr<ID3D11Buffer> groundFogCB;
	bool groundFogFailed = false;
	int32_t fogGroundMin[2]{};    ///< The ground map's placement this frame
	float fogPatternOffset[2]{};  ///< How far the resting fog has drifted with the weather's wind
	bool fogReady = false;        ///< The fog layer and ground height are valid this frame

	bool shaderCompileFailed = false;
	bool resourceCreateFailed = false;
	/** @brief Set by the Reset button on the UI thread, consumed on the render thread. */
	std::atomic<bool> queuedReset = true;
};
