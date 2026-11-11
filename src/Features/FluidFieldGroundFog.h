#pragma once

#include "Feature.h"
#include "FluidField/GroundTiles.h"

/**
 * @brief Ground Fog (D194): a thin heavy fog layer lying on a baked ground round the player, drawn as a volume.
 *
 * The ground comes in three tiers (GroundTiles): mesh tiles near the player, built on a worker and cached on disk, the
 * loaded landscape while they build, and the worldspace's height map far out. Runs only while this feature is on.
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

	struct Settings
	{
		bool Enabled = false;
		uint DebugView = 0;  ///< 1 colours every surface by what the fog's ground takes it for
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

	/** @brief Brings the ground up to date. */
	virtual void Prepass() override;

	/** @brief Drops the shaders so they recompile. */
	virtual void ClearShaderCache() override;

	/** @brief Loaded and enabled. */
	bool IsActive() const { return loaded && settings.Enabled; }

	/** @brief D21: draws over the finished world, before first person, TAA and tonemapping. */
	void DrawAfterWorld();

private:
	void Release();
	bool released = true;
	/** @brief Loading screens this feature has reset for (Field::ConsumeLoad). */
	uint32_t seenLoads = UINT32_MAX;

	void DrawDebug(ID3D11UnorderedAccessView* a_main, ID3D11ShaderResourceView* a_depth);

	/** @brief Matches GroundDebugCB in GroundDebugCS.hlsl. */
	struct alignas(16) GroundDebugConstants
	{
		FluidFieldSim::GroundTiles::SlotTile slotTiles[FluidFieldSim::GroundTiles::SLOTS * FluidFieldSim::GroundTiles::SLOTS];
		int32_t tileMin[2];
		float farScale[2];
		float farOffset[2];
		float farZ[2];
		uint32_t hasFar;
		float pad[3];
	};
	static_assert(sizeof(GroundDebugConstants) == 64 * 16 + 48);

	FluidFieldSim::GroundTiles ground;
	/** @brief Main-thread milliseconds of the ground update, smoothed (the UI shows it). */
	float groundMs = 0.0f;
	winrt::com_ptr<ID3D11ComputeShader> groundDebugCS;
	winrt::com_ptr<ID3D11Buffer> groundDebugCB;
	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	bool debugFailed = false;
};
