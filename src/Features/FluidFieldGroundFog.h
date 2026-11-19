#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "FluidField/AirSources.h"
#include "FluidField/FogSolver.h"
#include "FluidField/GroundTiles.h"
#include "FluidField/LandscapeDepth.h"

/**
 * @brief Ground Fog (D194): a thin heavy fog layer lying on a baked ground round the player, drawn as a volume.
 *
 * The ground comes in three tiers (GroundTiles): mesh tiles near the player, built on a worker and cached on disk, the
 * loaded landscape while they build, and the landscape's own depth far out. The fog flows over it (FogSolver): it forms
 * where it may, drains downhill, pools in hollows and spills off ledges but never climbs them; D195: on two levels where
 * the ground has two (under overhangs). Moving actors push it; close by, their legs cut it exactly (the contact map). Runs
 * only while this feature is on.
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
			{ T("feature.fluid_field_ground_fog.key_feature_1", "Drains downhill and pools in hollows"),
				T("feature.fluid_field_ground_fog.key_feature_2", "Spills off ledges and flows round rocks"),
				T("feature.fluid_field_ground_fog.key_feature_3", "Parted by moving actors, cut where their legs pass") } };
	}

	/** @brief Fog Detail's cell sizes, world units: the grid's reach changes, its cost does not. */
	static constexpr std::array<float, 3> CELL_SIZES = { 12.0f, 16.0f, 24.0f };
	static constexpr uint32_t GRID_RESOLUTION = 512;

	struct Settings
	{
		bool Enabled = false;
		float FogDensity = 0.04f;       ///< Extinction per world unit inside the fog
		float FogBrightness = 0.15f;    ///< How brightly the fog is lit, as a fraction of the light around it
		float GroundThickness = 20.0f;  ///< Fog depth on open ground where it forms, world units
		float PoolDepth = 40.0f;        ///< Deepest fog in hollows, world units
		float EaseIn = 20.0f;           ///< Seconds for fog to form back (about two thirds of the way)
		uint FogDetail = 1;             ///< CELL_SIZES index
		float ContactRadius = 18.0f;    ///< Metres round the player within which legs cut the fog exactly
		float Stir = 1.0f;              ///< How hard moving actors push the fog
		float DistantReach = 0.9f;      ///< How far the distant fog reaches, as a share of the LOD terrain's reach
		uint LocalLights = 1;           ///< Torches and fires glowing in the fog: 0 off, 1 the 4 closest to the camera, 2 the 8 closest
		uint Samples = 24;              ///< Samples along each view ray (more: finer shadow edges, more cost)
		uint DebugView = 0;             ///< 1 colours every surface by what the fog's ground takes it for, brighter where fog lies
		// Indoors (dungeons), their own tuning; GroundFogPlaces.json's per-kind multipliers apply on top
		float InteriorFogDensity = 0.04f;
		float InteriorFogBrightness = 0.15f;
		float InteriorGroundThickness = 20.0f;
		float InteriorPoolDepth = 40.0f;
		float InteriorEaseIn = 20.0f;
	};

	Settings settings;

	/** @brief The settings in force: indoors (in a dungeon), the interior tuning in place of the outdoor one. */
	Settings ActiveSettings() const;

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
	/** @brief Hooks explosions and listens for shouts (step 6 movers). */
	virtual void DataLoaded() override;

	/** @brief Brings the ground up to date and steps the fog. */
	virtual void Prepass() override;

	/** @brief Drops the shaders so they recompile. */
	virtual void ClearShaderCache() override;

	/** @brief Loaded and enabled. */
	bool IsActive() const { return loaded && settings.Enabled; }

	/** @brief D21: draws the fog over the finished world, before first person, TAA and tonemapping. */
	void DrawAfterWorld();

private:
	void Release();
	bool released = true;
	/** @brief Loading screens this feature has reset for (Field::ConsumeLoad). */
	uint32_t seenLoads = UINT32_MAX;
	/** @brief Creates the solver's resources and shaders; false if it cannot run. */
	bool EnsureSolver();
	FluidFieldSim::FogGround MakeGround() const;

	/** @brief Item 9: a dungeon kind fog forms in indoors (GroundFogPlaces.json), by its location's keywords. */
	struct Place
	{
		std::string name;
		std::vector<std::string> keywords;
		float amount = 1.0f;   ///< Times Ground Thickness
		float density = 1.0f;  ///< Times Density
		std::array<float, 3> tint{ 1.0f, 1.0f, 1.0f };
	};
	std::vector<Place> places;
	/** @brief The player's dungeon kind this frame indoors; null outdoors, or indoors where fog doesn't form. */
	const Place* currentPlace = nullptr;
	void LoadPlaces();
	const Place* FindPlace() const;
	/** @brief Indoors in a dungeon fog forms in. */
	bool InDungeon() const { return currentPlace != nullptr; }
	FluidFieldSim::FogLayerParams MakeLayer() const;

	void DrawFog(ID3D11Texture2D* a_mainTexture, ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth);
	void DrawDebug(ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth);

	/** @brief Matches GroundFogCB in GroundFogCS.hlsl. */
	struct alignas(16) GroundFogConstants
	{
		int32_t gridMin[2];
		uint32_t resolution;
		float cellSize;
		float density;
		uint32_t stepCount;
		float maxDistance;
		float maxDepth;
		float albedo[3];
		float farDepth;
		FluidFieldSim::GroundTiles::SlotTile slotTiles[FluidFieldSim::GroundTiles::SLOTS * FluidFieldSim::GroundTiles::SLOTS];
		int32_t tileMin[2];
		int32_t landCell[2];
		float vlMix;  ///< Volumetric Lighting's own colour's share over the sun's (4e)
		float hollowGain;
		float farPoolDepth;
		float contactCell;
		int32_t contactMin[2];
		uint32_t contactResolution;
		float farDistance;
		float vlPhase[2];  ///< Volumetric Lighting's phase: its scattering (anisotropy) and contribution (4e)
		uint32_t hasLod;
		float vlIntensity;
		uint32_t hasSunShadow;    ///< Volumetric Shadows' shadow map bound (t18, t98)
		uint32_t hasSky;          ///< Skylighting's probes bound (t14)
		uint32_t localLights;     ///< Light Limit Fix's lights bound (t35 to t37): the setting, else 0
		uint32_t hasSceneShadow;  ///< The game's sun shadow cascades bound (t23): actors' shadows near the camera (4d)
		float vlColour[3];        ///< Volumetric Lighting's own colour (4e)
		uint32_t hasVL;           ///< Volumetric Lighting is on: the fog's sun takes its colour, phase and strength
		uint32_t hasHistory;      ///< Last frame's fog is valid to blend with (the TEMPORAL pass)
		uint32_t lightCount;      ///< FogLights this frame
		float historyPad[2];
	};
	static_assert(sizeof(GroundFogConstants) == 48 + 256 * 16 + 112);

	/** @brief Matches GroundDebugCB in GroundDebugCS.hlsl. */
	struct alignas(16) GroundDebugConstants
	{
		FluidFieldSim::GroundTiles::SlotTile slotTiles[FluidFieldSim::GroundTiles::SLOTS * FluidFieldSim::GroundTiles::SLOTS];
		int32_t tileMin[2];
		int32_t landCell[2];
		uint32_t resolution;
		float cellSize;
		float poolDepth;
		float pad0;
		int32_t gridMin[2];
		float pad[2];
	};
	static_assert(sizeof(GroundDebugConstants) == 256 * 16 + 48);

	FluidFieldSim::GroundTiles ground;
	FluidFieldSim::FogSolver solver;
	/** @brief The distant fog's reach this frame, world units (Distant Reach of the drawn landscape's reach). */
	float DistantDistance() const;
	FluidFieldSim::AirSources airSources;
	int32_t contactMin[2]{};  ///< The contact map's placement this frame
	/** @brief World units a metre, for Contact Radius. */
	static constexpr float UNITS_PER_METRE = 70.0f;
	/** @brief The fog's drift at the weather's full wind speed, world units per second. */
	static constexpr float MAX_WIND_DRIFT = 80.0f;
	/** @brief The least drift any real wind gives, and the weather's wind value under which it is calm. */
	static constexpr float MIN_WIND_DRIFT = 15.0f;
	static constexpr float CALM_WIND = 0.01f;
	/** @brief How far the LOD fog (past the simulated fog) reaches, world units. */
	static constexpr float FAR_DISTANCE = 30000.0f;
	float fogTime = 0.0f;  ///< Seconds stepped, for the ambient drift
	/** @brief World units per second a running actor shoves the fog aside, at Stir 1. */
	static constexpr float SHOVE_SPEED = 150.0f;
	int32_t gridMin[2]{};
	float cellSize = 0.0f;  ///< The grid's cell size it was last stepped with; a change resets it
	bool fogReady = false;  ///< Stepped this frame
	bool solverFailed = false;
	/** @brief Main-thread milliseconds of the ground update, smoothed (the UI shows it). */
	float groundMs = 0.0f;
	winrt::com_ptr<ID3D11ComputeShader> groundFogCS;
	winrt::com_ptr<ID3D11ComputeShader> groundFogCompositeCS;  ///< Lays the half-resolution fog over the scene
	winrt::com_ptr<ID3D11ComputeShader> groundDebugCS;
	winrt::com_ptr<ID3D11Buffer> groundFogCB;
	winrt::com_ptr<ID3D11Buffer> groundDebugCB;
	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	/** @brief Tiling value noise for the fog (GroundFogCS), built once, kept in memory, and its wrapping sampler. */
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 3> noiseSRVs;  ///< Detail and clumps (3D), banks (2D)
	winrt::com_ptr<ID3D11SamplerState> wrapSampler;
	winrt::com_ptr<ID3D11SamplerState> shadowSampler;  ///< Comparison, for the game's sun shadow cascades (4d)
	bool EnsureNoise();
	std::unique_ptr<Texture2D> fogHalf;          ///< In-scattered light (rgb) and transmittance (a), half resolution
	std::unique_ptr<Texture2D> fogHalfDistance;  ///< Distance to each half-resolution texel's surface
	/** @brief 4a/4b: the half-resolution fog blended over frames (GroundFogCS TEMPORAL), ping-ponged, so the march's
	 * jitter averages out while the camera moves too (the game's TAA cannot resolve it then: stripes). */
	std::array<std::unique_ptr<Texture2D>, 2> fogHistory, fogHistoryDistance;

	/** @brief A light glowing in the fog (GroundFogCS FogLight). */
	struct FogLight
	{
		float position[3];  ///< Camera-relative
		float radius;
		float colour[3];  ///< Working space, times its fade and its share at the pick's cut
		float pad;
	};
	static constexpr uint32_t FEW_LIGHTS = 4;
	static constexpr uint32_t NEAR_LIGHTS = 8;
	std::unique_ptr<StructuredBuffer> fogLights;  ///< The lights closest to the camera, NEAR_LIGHTS at most
	uint32_t lightPickFrame = UINT32_MAX;         ///< The frame fogLights was picked for: once a frame, for the main camera
	uint32_t lightPickCount = 0;
	/** @brief Picks the scene's lights closest to the camera into fogLights; returns how many. */
	uint32_t PickFogLights();
	uint32_t historyIndex = 0;
	bool historyValid = false;
	float lastEye[3]{};
	winrt::com_ptr<ID3D11ComputeShader> groundFogTemporalCS;
	bool drawFailed = false;
	/** @brief The rays' bounds (GroundFogCS's TOP passes): the fog's highest top per 64-unit texel round the player, then
	 * dilated into fogTop's first mip, with max mips. */
	static constexpr uint32_t TOP_TEXELS = 512, TOP_MIPS = 7;
	static_assert(TOP_TEXELS == GRID_RESOLUTION, "GroundFogCS TOP 3 runs one thread a cell and a top map texel");
	winrt::com_ptr<ID3D11Texture2D> fogTopRaw, fogTop;
	winrt::com_ptr<ID3D11ShaderResourceView> fogTopRawSRV, fogTopSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> fogTopRawUAV;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, TOP_MIPS> fogTopMipSRVs;
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, TOP_MIPS> fogTopMipUAVs;
	std::array<winrt::com_ptr<ID3D11ComputeShader>, 4> fogTopCS;  ///< TOP 0 to 3
	/** @brief Items 3 and 4 (TOP 3): per cell and level (as the column), all the march reads there: drawA (floor, depth,
	 * velocity) and drawB (churn, carrying share, settled depth, sun and sky visibility packed), both RGBA32F; and the light per top map
	 * texel past the grid (lightFar, RG8). */
	winrt::com_ptr<ID3D11Texture2D> drawA, drawB, lightFar;
	winrt::com_ptr<ID3D11ShaderResourceView> drawASRV, drawBSRV, lightFarSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> drawAUAV, drawBUAV, lightFarUAV;
	bool EnsureFogTop();
	void ReleaseFogTop();
	void BuildFogTop(ID3D11DeviceContext* a_context);
	/** @brief Set by the Reset button on the UI thread, consumed on the render thread. */
	std::atomic<bool> queuedReset = true;
};
