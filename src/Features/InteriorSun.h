#pragma once
#include "Buffer.h"
#include "ShaderCache.h"

struct InteriorSun : Feature
{
public:
	virtual inline std::string GetName() override { return "Interior Sun"; }
	virtual std::string GetDisplayName() override { return T("feature.interior_sun.name", "Interior Sun"); }
	virtual inline std::string GetShortName() override { return "InteriorSun"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.interior_sun.description", "Allows for the sun and moon to cast light and shadows into interior spaces."),
			{ T("feature.interior_sun.key_feature_1", "Functions only for explicitly enabled interiors"),
				T("feature.interior_sun.key_feature_2", "Utilizes existing sun, moon, and weather systems"),
				T("feature.interior_sun.key_feature_3", "Includes an option to force double-sided rendering for unprepared interiors"),
				T("feature.interior_sun.key_feature_4", "Fixes geometry culling issues that cause light leakage") } };
	};

	/** @brief Draws the ImGui settings UI for Interior Sun configuration. */
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	/** @brief Installs rendering hooks and patches for interior sun/shadow support. */
	virtual void PostPostLoad() override;
	/** @brief Updates the interior sun state based on the current cell during the early prepass. */
	virtual void EarlyPrepass() override;
	/** @brief Creates the GPU resources used to draw window glass into the sun shadow maps. */
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual bool HasDebugSection() const override { return true; }
	virtual void DrawDebugSection() override;

	/** @brief Registers the menu open/close event handler for tracking main menu state. */
	virtual void DataLoaded() override
	{
		MenuOpenCloseEventHandler::Register();
	}

	struct Settings
	{
		bool ForceDoubleSidedRendering = true;
		float InteriorShadowDistance = 5000;
		bool WindowLightOnly = true;
		bool DetectOpenings = true;
		bool HideFakeWindowGlows = true;
		bool SkyLitGlass = false;
		float SkyLitGlassStrength = 1.0f;
		bool AutoSunlitInteriors = false;
	};

	Settings settings;

	std::atomic<bool> isInteriorWithSun = false;

	/** @brief Hook that intercepts world space queries to enable/disable interior sun based on cell flags. */
	struct GetWorldSpace
	{
		static RE::TESWorldSpace* thunk(RE::TES* tes);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that overrides directional shadow light culling to include rooms visible from the sun direction. */
	struct DirShadowLightCulling
	{
		static void thunk(RE::BSShadowDirectionalLight* dirLight, RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>& jobArrays, RE::BSTArray<RE::NiPointer<RE::NiAVObject>>& nodes);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that updates the raster cull mode before each render pass to support double-sided rendering. */
	struct BSBatchRenderer_RenderPassImmediately
	{
		static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that runs before a camera's accumulated geometry is drawn, once its targets are bound and cleared. */
	struct BSShaderAccumulator_StartAccumulating
	{
		static void thunk(RE::BSShaderAccumulator* a_accumulator, RE::NiCamera* a_camera);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief Conditionally disables backface culling for shadow rendering in interior sun cells.
	 * @param pass The current render pass being processed.
	 * @param technique The shader technique flags for the pass.
	 */
	void UpdateRasterStateCullMode(const RE::BSRenderPass* pass, const uint32_t technique) const
	{
		if (isInteriorWithSun && settings.ForceDoubleSidedRendering && technique & static_cast<uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderShadowmap)) {
			const auto flags = pass->shaderProperty->flags;
			const auto renderTwoSided = flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided) || flags.none(RE::BSShaderProperty::EShaderPropertyFlag::kAssumeShadowmask, RE::BSShaderProperty::EShaderPropertyFlag::kSkinned);
			if (renderTwoSided && *rasterStateCullMode != 0) {
				*rasterStateCullMode = 0;
				globals::game::stateUpdateFlags->set(RE::BSGraphics::DIRTY_RASTER_CULL_MODE);
			} else if (!renderTwoSided && *rasterStateCullMode != RE::BSGraphics::RasterStateCullMode::RASTER_STATE_CULL_MODE_BACK) {
				*rasterStateCullMode = RE::BSGraphics::RasterStateCullMode::RASTER_STATE_CULL_MODE_BACK;
				globals::game::stateUpdateFlags->set(RE::BSGraphics::DIRTY_RASTER_CULL_MODE);
			}
		}
	}

	/** @brief Event handler that resets interior sun state when the main menu opens. */
	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		/** @brief Processes menu open/close events to reset interior sun state on main menu. */
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*);

		/** @brief Registers the singleton event handler with the UI event source. */
		static bool Register()
		{
			static MenuOpenCloseEventHandler singleton;
			auto ui = globals::game::ui;
			if (!ui) {
				logger::error("UI event source not found");
				return false;
			}
			ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);

			logger::info("Registered {}", typeid(singleton).name());
			return true;
		}
	};

	/**
	 * @brief Checks whether a cell is an interior with sunlight shadow flags enabled.
	 * @param cell The cell to check.
	 * @return True if the cell is an interior configured for sun and sky lighting with sunlight shadows.
	 */
	static bool IsInteriorWithSun(const RE::TESObjectCELL* cell);
	/** @brief Returns whether this feature is loaded and the current cell has interior sun enabled. */
	bool IsActiveInteriorSun() const { return loaded && isInteriorWithSun.load(); }
	virtual bool IsCore() const override { return true; };

private:
	enum class CellFlagExt : uint16_t
	{
		kSunlightShadows = 1 << 15,
	};

	float* gShadowDistance = nullptr;
	float* gInteriorShadowDistance = nullptr;
	uint32_t* rasterStateCullMode = nullptr;

	RE::TESObjectCELL* currentCell = nullptr;
	RE::NiPointer<RE::BSPortalGraph> currentPortalGraph = nullptr;

	bool arraysCleared = true;
	RE::BSTArray<RE::NiPointer<RE::NiAVObject>> currentCellRoomsAndPortals = {};
	RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>> replacementJobArrays = {};
	eastl::hash_set<RE::NiAVObject*> addedSet = {};

	static RE::TESWorldSpace* enableInteriorSun;
	static RE::TESWorldSpace* disableInteriorSun;

	void ClearArrays();

	void InitialiseOnNewCell(const RE::NiPointer<RE::BSPortalGraph>& portalGraph);

	bool IsInSunDirectionAndWithinShadowDistance(const RE::NiPointer<RE::NiAVObject>& object, const RE::NiPoint3& lightDir, const RE::NiPoint3& playerPos) const;

	void PopulateReplacementJobArrays(RE::TESObjectCELL* cell, const RE::NiPointer<RE::BSPortalGraph>& portalGraph, const RE::BSShadowDirectionalLight* dirLight, RE::BSTArray<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>& jobArrays);

	static void SetShadowDistance(bool inInterior);

	struct alignas(16) WindowHolesCB
	{
		float4x4 ViewProj;
		float3 PosAdjust;
		float pad0;
	};

	// Rescans after entering a cell, in frames: its 3D can still be loading on the first ones
	static constexpr uint32_t WindowGlassScanFrames[] = { 1, 60, 300 };

	/** @brief A see-through shape that may be an opening in the cell's shell, and what the shell test found. */
	struct OpeningCandidate
	{
		std::string model;
		std::string shape;
		RE::FormID ref = 0;
		uint32_t firstTriangle = 0;
		uint32_t triangleCount = 0;
		uint32_t openSamples = 0;
		uint32_t samples = 0;
		float nearSide = 0.0f;  // mean distance to the room on the side that hits something
		bool opening = false;
	};

	/** @brief Shell test of one cell's candidates, run on a worker thread on copies of the cell's triangles. */
	struct OpeningScan
	{
		RE::TESObjectCELL* cell = nullptr;
		std::vector<float3> occluders;           // triangle list of everything that blocks light
		std::vector<uint32_t> occluderShapes;    // first triangle of each occluder shape, plus the end
		std::vector<float3> candidateTriangles;  // triangle list of the candidates
		std::vector<OpeningCandidate> candidates;
		std::vector<float3> openings;  // result: triangle list of the shapes found to be openings
		float milliseconds = 0.0f;
		std::atomic<bool> done = false;
	};

	struct GlassEmissive
	{
		RE::NiPointer<RE::BSLightingShaderProperty> property;
		RE::NiColor color;
		float mult;
	};

	std::thread::id renderThread;
	RE::TESObjectCELL* windowGlassCell = nullptr;
	uint32_t windowGlassCellFrames = 0;
	uint32_t windowGlassVertexCount = 0;
	uint32_t windowGlassShapes = 0;
	uint32_t windowGlassShapesSkipped = 0;
	uint32_t windowHolesDrawn = 0;
	uint32_t windowHolesDrawnLastFrame = 0;
	uint32_t windowHolesTargetMismatches = 0;
	// Holes drawn per depth target, this frame and the last: the sun cascades and volumetric lighting's maps apart
	std::array<uint32_t, RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL> windowHolesByTarget{};
	std::array<uint32_t, RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL> windowHolesByTargetLastFrame{};
	float windowGlassScanMilliseconds = 0.0f;

	std::shared_ptr<OpeningScan> openingScan;  // in flight or finished; dropped on leaving the cell
	std::shared_ptr<std::vector<float3>> lastCandidateTriangles;
	bool glassEmissivesLit = false;
	std::vector<float3> windowOpeningVertices;
	uint32_t openingCandidates = 0;
	uint32_t openingShapes = 0;
	float openingScanMilliseconds = 0.0f;

	std::vector<RE::NiPointer<RE::NiAVObject>> hiddenFakeGlows;
	std::vector<GlassEmissive> glassEmissives;
	std::unordered_map<RE::TESObjectCELL*, uint16_t> autoSunlitCells;  // cells given the sun, with their own flags to restore

	std::vector<float3> windowGlassVertices;
	std::unique_ptr<Buffer> windowGlass;
	ConstantBuffer* windowHolesCB = nullptr;
	winrt::com_ptr<ID3D11VertexShader> windowHolesVS;
	winrt::com_ptr<ID3D11RasterizerState> windowHolesRasterState;
	winrt::com_ptr<ID3D11DepthStencilState> windowHolesDepthState;

	ID3D11VertexShader* GetWindowHolesVS();

	void UpdateWindowGlass(RE::TESObjectCELL* cell);

	void CollectWindowGlass(RE::TESObjectCELL* cell);

	/** @brief Rebuilds the GPU triangle list from the glass and the openings found so far. */
	void UploadWindowHoles(const RE::TESObjectCELL* cell);

	/** @brief Takes the result of a finished opening scan, if any. */
	void TakeOpeningScan();

	/** @brief The shell test: marks the candidates that have the room on one side and nothing on the other. */
	static void RunOpeningScan(OpeningScan& scan);

	/** @brief Gives a cell with windows or openings the sun, unless it is a cave or dungeon. */
	void UpdateAutoSunlit(RE::TESObjectCELL* cell);

	/** @brief Puts back the flags of every cell given the sun automatically. */
	void RestoreAutoSunlitCells();

	void RestoreFakeGlows();

	void RestoreGlassEmissives();

	/** @brief Lights the glass from the sky's colour while the cell has the sun. */
	void UpdateSkyLitGlass();

	void ReleaseWindowGlass();

	/** @brief Turns the bound sun shadow map into "all shadowed" with a hole at each window pane. */
	void DrawWindowHoles(const RE::NiCamera* camera);
};
