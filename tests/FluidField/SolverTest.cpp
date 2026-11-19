// Fluid Field fog solver referee.
//
// Runs the real FogSolver and its HLSL on a WARP (software) D3D11 device over made-up ground and checks the layer's rules
// (fog-v3.md): fog is conserved, a bowl fills level up to Pool Depth, fog spills off a ledge but never climbs onto it,
// rocks take fog only from higher ground, a push conserves fog, fog under an overhang lies apart from the fog on it, and
// where no tile is resident the bed is the loaded landscape.
//
// Exit code 0 = every case passed, 1 = a case failed, 2 = setup failure.

#include "FluidField/FogSolver.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#if !defined(FLUIDFIELD_SHADER_DIR) || !defined(FLUIDFIELD_COMMON_SHADER_DIR)
#	error "FLUIDFIELD_SHADER_DIR and FLUIDFIELD_COMMON_SHADER_DIR must point at the Ground Fog and the common shaders"
#endif

namespace
{
	using FluidFieldSim::FogGround;
	using FluidFieldSim::FogLayerParams;
	using FluidFieldSim::FogMovers;
	using FluidFieldSim::FogSolver;
	using FluidFieldSim::Mover;
	using FluidFieldSim::StepParams;

	constexpr std::uint32_t N = 64;     // Cells a side
	constexpr float CELL = 16.0f;       // 2 x 2 ground texels a cell
	constexpr int TEXELS = 1024;        // The ground tiles' resident texture
	constexpr float DT = 1.0f / 30.0f;  // The longest frame the game steps
	enum : std::uint32_t
	{
		MAY_FORM = 1,
		ROCK = 2
	};

	/** @brief Resolves `#include "FluidField/..."` against the features' shader roots, which the game merges into one folder. */
	class ShaderInclude final : public ID3DInclude
	{
	public:
		explicit ShaderInclude(std::vector<std::filesystem::path> a_roots) :
			roots(std::move(a_roots)) {}

		HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR fileName, LPCVOID, LPCVOID* data, UINT* bytes) override
		{
			std::ifstream file;
			for (const auto& root : roots) {
				file.open(root / fileName, std::ios::binary);
				if (file.is_open())
					break;
				file.clear();
			}
			if (!file.is_open())
				return E_FAIL;
			auto* buffer = new std::string(std::istreambuf_iterator<char>(file), {});
			*data = buffer->data();
			*bytes = static_cast<UINT>(buffer->size());
			open.push_back(buffer);
			return S_OK;
		}

		HRESULT __stdcall Close(LPCVOID data) override
		{
			auto it = std::ranges::find_if(open, [&](std::string* s) { return s->data() == data; });
			if (it != open.end()) {
				delete *it;
				open.erase(it);
			}
			return S_OK;
		}

	private:
		std::vector<std::filesystem::path> roots;
		std::vector<std::string*> open;
	};

	struct Gpu
	{
		winrt::com_ptr<ID3D11Device> device;
		winrt::com_ptr<ID3D11DeviceContext> context;
	};

	ID3D11ComputeShader* CompileShader(ID3D11Device* device, const std::vector<std::filesystem::path>& roots, const wchar_t* fileName)
	{
		ShaderInclude include(roots);
		winrt::com_ptr<ID3DBlob> blob, errors;
		const auto path = roots.front() / L"FluidField" / fileName;
		const HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, &include, "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.put(), errors.put());
		if (FAILED(hr)) {
			std::fprintf(stderr, "Compile failed: %ls\n%s\n", path.c_str(), errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
			return nullptr;
		}
		ID3D11ComputeShader* shader = nullptr;
		if (FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader)))
			return nullptr;
		return shader;
	}

	using GroundFn = std::function<std::pair<float, std::uint32_t>(float, float)>;

	/** @brief One run: the top (height, flags) per 8-unit texel from a_ground, and the floor under an overhang from a_lower. */
	struct Run
	{
		const Gpu& gpu;
		FogSolver& solver;
		winrt::com_ptr<ID3D11Texture2D> tiles;
		winrt::com_ptr<ID3D11ShaderResourceView> tilesSRV;
		FogGround ground;

		Run(const Gpu& a_gpu, FogSolver& a_solver, const GroundFn& a_ground, const GroundFn& a_lower = {}) :
			gpu(a_gpu), solver(a_solver)
		{
			std::vector<float> texels(static_cast<std::size_t>(TEXELS) * TEXELS * 4);
			for (int y = 0; y < TEXELS; y++)
				for (int x = 0; x < TEXELS; x++) {
					const float wx = (x + 0.5f) * 8.0f, wy = (y + 0.5f) * 8.0f;
					const auto [height, flags] = a_ground(wx, wy);
					const auto [lower, lowerFlags] = a_lower ? a_lower(wx, wy) : std::pair{ -1.0e7f, std::uint32_t{ 0 } };
					float* texel = &texels[(static_cast<std::size_t>(y) * TEXELS + x) * 4];
					texel[0] = height;
					texel[1] = static_cast<float>(flags);
					texel[2] = lower;
					texel[3] = static_cast<float>(lowerFlags);
				}
			const D3D11_TEXTURE2D_DESC desc{ .Width = TEXELS, .Height = TEXELS, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = 0 };
			const D3D11_SUBRESOURCE_DATA data{ .pSysMem = texels.data(), .SysMemPitch = TEXELS * 4 * sizeof(float), .SysMemSlicePitch = 0 };
			winrt::check_hresult(gpu.device->CreateTexture2D(&desc, &data, tiles.put()));
			winrt::check_hresult(gpu.device->CreateShaderResourceView(tiles.get(), nullptr, tilesSRV.put()));
			ground.tiles = tilesSRV.get();
			for (int i = 0; i < 256; i++) {
				ground.slots[i * 4] = i % 16;
				ground.slots[i * 4 + 1] = i / 16;
				ground.slots[i * 4 + 2] = 1;
			}
			solver.Reset();
		}

		void Step(const FogLayerParams& a_layer, int a_steps, std::span<const Mover> a_pushers = {})
		{
			const StepParams grid{ .gridMin = { 0, 0 }, .cellSize = CELL, .deltaTime = DT };
			for (int i = 0; i < a_steps; i++)
				solver.Step(gpu.context.get(), grid, a_layer, ground, FogMovers{ .pushers = a_pushers });
		}

		/** @brief A level's depth per cell. */
		std::vector<float> Depth(std::uint32_t a_level = 0) const { return Read(solver.GetDepthTexture(), 1, a_level); }

		/** @brief The texture's first channel per cell (a_channels floats a texel) in one array slice. */
		std::vector<float> Read(ID3D11Texture2D* a_texture, std::uint32_t a_channels, std::uint32_t a_slice) const
		{
			D3D11_TEXTURE2D_DESC desc{};
			a_texture->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			winrt::com_ptr<ID3D11Texture2D> staging;
			winrt::check_hresult(gpu.device->CreateTexture2D(&desc, nullptr, staging.put()));
			gpu.context->CopyResource(staging.get(), a_texture);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			winrt::check_hresult(gpu.context->Map(staging.get(), a_slice, D3D11_MAP_READ, 0, &mapped));
			std::vector<float> out(static_cast<std::size_t>(N) * N);
			for (std::uint32_t y = 0; y < N; y++)
				for (std::uint32_t x = 0; x < N; x++)
					out[y * N + x] = reinterpret_cast<const float*>(static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch)[x * a_channels];
			gpu.context->Unmap(staging.get(), a_slice);
			return out;
		}
	};

	float Sum(const std::vector<float>& a_depth)
	{
		double sum = 0.0;
		for (const float d : a_depth)
			sum += d;
		return static_cast<float>(sum);
	}

	bool Check(const char* a_case, bool a_pass, const std::string& a_detail)
	{
		std::printf("%-40s %s  %s\n", a_case, a_pass ? "ok  " : "FAIL", a_detail.c_str());
		return a_pass;
	}

	/** @brief Only gravity, damping and the fog it starts with. */
	FogLayerParams Closed()
	{
		FogLayerParams layer;
		layer.sourceRate = 0.0f;
		layer.evaporateRate = 0.0f;
		layer.drift = 0.0f;
		return layer;
	}

	std::size_t At(int x, int y) { return static_cast<std::size_t>(y) * N + x; }
}

int main(int argc, char** argv)
{
	const std::vector<std::filesystem::path> roots = argc > 1 ? std::vector<std::filesystem::path>{ argv[1] } : std::vector<std::filesystem::path>{ FLUIDFIELD_SHADER_DIR, FLUIDFIELD_COMMON_SHADER_DIR };
	Gpu gpu;
	const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, gpu.device.put(), nullptr, gpu.context.put()))) {
		std::fprintf(stderr, "No WARP device\n");
		return 2;
	}
	FogSolver solver;
	try {
		solver.CreateResources(gpu.device.get(), N, nullptr);
	} catch (const winrt::hresult_error&) {
		std::fprintf(stderr, "Could not create the solver's textures\n");
		return 2;
	}
	if (!solver.CompileShaders([&](const wchar_t* name) { return CompileShader(gpu.device.get(), roots, name); }))
		return 2;

	bool pass = true;
	const float centre = N * CELL * 0.5f;

	// 1. Conservation: a bowl, its fog sloshing round with nothing added or taken
	{
		Run run(gpu, solver, [&](float x, float y) { return std::pair{ 0.002f * ((x - centre) * (x - centre) + (y - centre) * (y - centre)), std::uint32_t{ MAY_FORM } }; });
		run.Step(Closed(), 1);
		const float before = Sum(run.Depth(0)) + Sum(run.Depth(1));
		run.Step(Closed(), 300);
		const float after = Sum(run.Depth(0)) + Sum(run.Depth(1));
		pass &= Check("1. conserved", before > 0.0f && std::abs(after - before) <= 1e-3f * before, std::format("{:.2f} -> {:.2f}", before, after));
	}

	// 2. A bowl fills level and stops at Pool Depth: the settled fog drains into it, the excess evaporates
	{
		FogLayerParams layer = Closed();
		layer.evaporateRate = 3.0f;
		layer.gravity = 300.0f;  // Stiffer than the game's, so it levels within the run
		layer.fluxDamping = 1.5f;
		Run run(gpu, solver, [&](float x, float y) { return std::pair{ 0.0005f * ((x - centre) * (x - centre) + (y - centre) * (y - centre)), std::uint32_t{ MAY_FORM } }; });
		run.Step(layer, 1500);
		const auto depth = run.Depth();
		const float middle = depth[At(N / 2, N / 2)];
		// The pool: cells whose ground lies under the middle's surface; its surface level across them
		float lowest = 1e30f, highest = -1e30f;
		for (int y = 0; y < static_cast<int>(N); y++)
			for (int x = 0; x < static_cast<int>(N); x++) {
				const float wx = (x + 0.5f) * CELL - centre, wy = (y + 0.5f) * CELL - centre;
				const float bed = 0.0005f * (wx * wx + wy * wy);
				if (bed < middle - 10.0f) {
					lowest = std::min(lowest, bed + depth[At(x, y)]);
					highest = std::max(highest, bed + depth[At(x, y)]);
				}
			}
		pass &= Check("2. bowl fills to Pool Depth", middle > layer.poolDepth * 0.85f && middle < layer.poolDepth * 1.15f, std::format("middle {:.1f} (pool {:.0f})", middle, layer.poolDepth));
		pass &= Check("2. pool surface level", highest - lowest < 4.0f, std::format("surface spread {:.2f}", highest - lowest));
	}

	// 3. A ledge 100 units high along x = centre: fog on the floor never climbs it; fog on top spills over the brink
	{
		const auto ledge = [&](std::uint32_t a_topFlags, std::uint32_t a_floorFlags) {
			return [=](float x, float) { return x < centre ? std::pair{ 100.0f, a_topFlags } : std::pair{ 0.0f, a_floorFlags }; };
		};
		{
			FogLayerParams layer = Closed();
			layer.groundThickness = 60.0f;  // Deep enough to climb it if it could
			layer.poolDepth = 60.0f;
			Run run(gpu, solver, ledge(0, MAY_FORM));
			run.Step(layer, 300);
			const auto depth = run.Depth();
			float onTop = 0.0f;
			for (int y = 0; y < static_cast<int>(N); y++)
				for (int x = 0; x < static_cast<int>(N) / 2; x++)
					onTop = std::max(onTop, depth[At(x, y)]);
			pass &= Check("3. floor fog never climbs the ledge", onTop < 1e-4f, std::format("most on top {:.5f}", onTop));
		}
		{
			Run run(gpu, solver, ledge(MAY_FORM, 0));
			run.Step(Closed(), 150);
			const auto depth = run.Depth();
			const float below = depth[At(N / 2 + 1, N / 2)];
			pass &= Check("3. top fog spills over the brink", below > 0.5f, std::format("at the foot {:.2f}", below));
		}
	}

	// 4. A rock 30 high: deep fog beside it never enters; fog from a plateau higher than its top lands on it
	{
		const float rockMin = centre - 64.0f, rockMax = centre + 64.0f;
		const auto inRock = [&](float x, float y) { return x > rockMin && x < rockMax && y > rockMin && y < rockMax; };
		{
			FogLayerParams layer = Closed();
			layer.groundThickness = 50.0f;
			layer.poolDepth = 50.0f;
			Run run(gpu, solver, [&](float x, float y) { return inRock(x, y) ? std::pair{ 30.0f, std::uint32_t{ ROCK } } : std::pair{ 0.0f, std::uint32_t{ MAY_FORM } }; });
			run.Step(layer, 300);
			const auto depth = run.Depth();
			pass &= Check("4. fog beside a rock never enters", depth[At(N / 2, N / 2)] < 1e-4f, std::format("on the rock {:.5f}", depth[At(N / 2, N / 2)]));
		}
		{
			// The plateau (west of the rock, 60 high) carries fog; the floor round both carries none
			Run run(gpu, solver, [&](float x, float y) {
				if (inRock(x, y))
					return std::pair{ 30.0f, std::uint32_t{ ROCK } };
				if (x <= rockMin && x > rockMin - 160.0f && y > rockMin && y < rockMax)
					return std::pair{ 60.0f, std::uint32_t{ MAY_FORM } };
				return std::pair{ 0.0f, std::uint32_t{ 0 } };
			});
			run.Step(Closed(), 60);
			const auto depth = run.Depth();
			float onRock = 0.0f;
			for (int y = 0; y < static_cast<int>(N); y++)
				for (int x = 0; x < static_cast<int>(N); x++)
					if (inRock((x + 0.5f) * CELL, (y + 0.5f) * CELL))
						onRock = std::max(onRock, depth[At(x, y)]);
			pass &= Check("4. higher fog lands on the rock", onRock > 0.1f, std::format("most on the rock {:.2f}", onRock));
		}
	}

	// 5. A push conserves fog: it piles in front of the mover and thins behind it
	{
		Run run(gpu, solver, [&](float, float) { return std::pair{ 0.0f, std::uint32_t{ MAY_FORM } }; });
		run.Step(Closed(), 1);
		const float before = Sum(run.Depth());
		const Mover mover{ .from = { centre - 8.0f, centre }, .to = { centre + 8.0f, centre }, .radius = 24.0f, .shove = 150.0f, .velocity = { 200.0f, 0.0f }, .cut = 0.0f, .bottom = 0.0f, .top = 100.0f, .pad = 0.0f };
		run.Step(Closed(), 20, std::span(&mover, 1));
		const auto depth = run.Depth();
		const float after = Sum(depth);
		const float ahead = depth[At(N / 2 + 3, N / 2)], behind = depth[At(N / 2 - 2, N / 2)];
		pass &= Check("5. a push conserves fog", std::abs(after - before) <= 1e-3f * before, std::format("{:.2f} -> {:.2f}", before, after));
		pass &= Check("5. piles ahead, thins behind", ahead > 20.5f && behind < 19.5f, std::format("ahead {:.2f}, behind {:.2f}", ahead, behind));
	}

	// 6. An overhang 100 high over the middle third, the floor running on under it: fog lies under it, none climbs onto it,
	//    and a mover on top leaves the fog under it alone
	{
		const auto overhang = [&](float x) { return x > centre - 160.0f && x < centre + 160.0f; };
		Run run(
			gpu, solver, [&](float x, float) { return overhang(x) ? std::pair{ 100.0f, std::uint32_t{ 0 } } : std::pair{ 0.0f, std::uint32_t{ MAY_FORM } }; },
			[&](float x, float) { return overhang(x) ? std::pair{ 0.0f, std::uint32_t{ MAY_FORM } } : std::pair{ -1.0e7f, std::uint32_t{ 0 } }; });
		run.Step(Closed(), 120);
		const float under = run.Depth(1)[At(N / 2, N / 2)];
		float onTop = 0.0f;
		for (int x = 0; x < static_cast<int>(N); x++)
			onTop = std::max(onTop, overhang((x + 0.5f) * CELL) ? run.Depth(0)[At(x, N / 2)] : 0.0f);
		pass &= Check("6. fog lies under the overhang", under > 15.0f, std::format("under it {:.2f}", under));
		pass &= Check("6. none climbs onto it", onTop < 1e-4f, std::format("most on top {:.5f}", onTop));
		const Mover above{ .from = { centre - 8.0f, centre }, .to = { centre + 8.0f, centre }, .radius = 24.0f, .shove = 150.0f, .velocity = { 200.0f, 0.0f }, .cut = 0.0f, .bottom = 100.0f, .top = 180.0f, .pad = 0.0f };
		run.Step(Closed(), 20, std::span(&above, 1));
		const auto stirred = run.Depth(1);
		Run still(
			gpu, solver, [&](float x, float) { return overhang(x) ? std::pair{ 100.0f, std::uint32_t{ 0 } } : std::pair{ 0.0f, std::uint32_t{ MAY_FORM } }; },
			[&](float x, float) { return overhang(x) ? std::pair{ 0.0f, std::uint32_t{ MAY_FORM } } : std::pair{ -1.0e7f, std::uint32_t{ 0 } }; });
		still.Step(Closed(), 140);
		const auto alone = still.Depth(1);
		float moved = 0.0f;
		for (std::size_t i = 0; i < alone.size(); i++)
			moved = std::max(moved, std::abs(stirred[i] - alone[i]));
		pass &= Check("6. a mover on top leaves the fog under it", moved < 0.05f, std::format("most change under it {:.4f}", moved));
	}

	// 7. Where no tile is resident, the bed is the loaded landscape's (the land map: 32-unit texels, 4,096-unit cells)
	{
		Run run(gpu, solver, [&](float, float) { return std::pair{ 0.0f, std::uint32_t{ MAY_FORM } }; });
		run.ground.slots = {};
		const auto landHeight = [](float x, float y) { return 300.0f + 0.25f * x - 0.1f * y; };
		std::vector<float> land(1024 * 1024 * 2);
		for (int y = 0; y < 1024; y++)
			for (int x = 0; x < 1024; x++) {
				land[(static_cast<std::size_t>(y) * 1024 + x) * 2] = landHeight((x + 0.5f) * 32.0f, (y + 0.5f) * 32.0f);
				land[(static_cast<std::size_t>(y) * 1024 + x) * 2 + 1] = static_cast<float>(MAY_FORM);
			}
		const D3D11_TEXTURE2D_DESC desc{ .Width = 1024, .Height = 1024, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R32G32_FLOAT, .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .CPUAccessFlags = 0, .MiscFlags = 0 };
		const D3D11_SUBRESOURCE_DATA data{ .pSysMem = land.data(), .SysMemPitch = 1024 * 2 * sizeof(float), .SysMemSlicePitch = 0 };
		winrt::com_ptr<ID3D11Texture2D> landTexture;
		winrt::com_ptr<ID3D11ShaderResourceView> landSRV;
		winrt::check_hresult(gpu.device->CreateTexture2D(&desc, &data, landTexture.put()));
		winrt::check_hresult(gpu.device->CreateShaderResourceView(landTexture.get(), nullptr, landSRV.put()));
		run.ground.land = landSRV.get();
		run.Step(Closed(), 1);
		const auto bed = run.Read(solver.GetBedTexture(), 4, 0);
		// From the second cell in: the first's bilinear reaches texel -1, which wraps to the map's far side
		float worst = 0.0f;
		for (std::uint32_t y = 1; y < N; y++)
			for (std::uint32_t x = 1; x < N; x++)
				worst = std::max(worst, std::abs(bed[y * N + x] - landHeight((x + 0.5f) * CELL, (y + 0.5f) * CELL)));
		pass &= Check("7. no tile: the bed is the land map's", worst < 0.01f, std::format("worst error {:.4f}", worst));
	}

	std::printf("%s\n", pass ? "All cases passed" : "Some cases FAILED");
	return pass ? 0 : 1;
}
