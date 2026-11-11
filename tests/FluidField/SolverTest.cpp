// Fluid Field solver referee.
//
// Runs the real WindSolver and its HLSL on a WARP (software) D3D11 device and compares
// every layer against a plain CPU implementation of the same stable-fluids step. The CPU
// version stores the grid in logical (non-wrapped) order and scrolls by shifting arrays,
// so a match also checks the GPU's toroidal addressing.
//
// Exit code 0 = every case passed, 1 = a mismatch, 2 = setup failure.

#include "FluidField/WindSolver.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if !defined(FLUIDFIELD_SHADER_DIR) || !defined(FLUIDFIELD_COMMON_SHADER_DIR)
#	error "FLUIDFIELD_SHADER_DIR and FLUIDFIELD_COMMON_SHADER_DIR must point at the Ground Fog and Shallow Water shaders"
#endif

namespace
{
	using FluidFieldSim::Mover;
	using FluidFieldSim::StepParams;
	using FluidFieldSim::WindSolver;

	constexpr float TOLERANCE = 1e-4f;  // Max error relative to the layer's largest CPU magnitude

	// ---------------------------------------------------------------------------------------
	// CPU reference
	// ---------------------------------------------------------------------------------------

	struct Vec2
	{
		float x = 0.0f;
		float y = 0.0f;
	};

	/** @brief Plain stable-fluids step on logical arrays; mirrors the HLSL passes one to one. */
	class CpuReference
	{
	public:
		explicit CpuReference(int a_n) :
			n(a_n), velocity(Size()), divergence(Size()), pressure(Size()) {}

		void Step(const StepParams& p)
		{
			const int prev[2] = { historyValid ? prevGridMin[0] : p.gridMin[0], historyValid ? prevGridMin[1] : p.gridMin[1] };

			// Add forces: keep texels whose cell was in the previous window, at its shifted index
			std::vector<Vec2> forced(Size());
			for (int y = 0; y < n; y++) {
				for (int x = 0; x < n; x++) {
					const int px = p.gridMin[0] + x - prev[0];
					const int py = p.gridMin[1] + y - prev[1];
					Vec2 v{};
					if (historyValid && px >= 0 && px < n && py >= 0 && py < n) {
						v = velocity[Index(px, py)];
						// Vorticity confinement (AddForcesCS), on the previous step's texels
						const float curl = Curl(p, prev, x, y);
						const float gx = std::abs(Curl(p, prev, x + 1, y)) - std::abs(Curl(p, prev, x - 1, y));
						const float gy = std::abs(Curl(p, prev, x, y + 1)) - std::abs(Curl(p, prev, x, y - 1));
						const float magnitude = std::sqrt(gx * gx + gy * gy);
						if (magnitude > 1e-6f) {
							v.x += VORTICITY * p.cellSize * curl * (gy / magnitude) * p.deltaTime;
							v.y += VORTICITY * p.cellSize * curl * (-gx / magnitude) * p.deltaTime;
						}
					}

					// Movers (SplatCS): point movers here, inside each one's footprint, in 1/256 fixed point
					for (const Mover& m : p.movers) {
						const float r = std::max(m.radius, 0.75f * p.cellSize);
						const float spread = (m.radius * m.radius) / (r * r);
						const int loX = std::max(static_cast<int>(std::floor((m.from[0] - 2.5f * r) / p.cellSize)) - p.gridMin[0], 0);
						const int loY = std::max(static_cast<int>(std::floor((m.from[1] - 2.5f * r) / p.cellSize)) - p.gridMin[1], 0);
						const int hiX = std::min(static_cast<int>(std::floor((m.from[0] + 2.5f * r) / p.cellSize)) - p.gridMin[0], n - 1);
						const int hiY = std::min(static_cast<int>(std::floor((m.from[1] + 2.5f * r) / p.cellSize)) - p.gridMin[1], n - 1);
						if (x < loX || x > hiX || y < loY || y > hiY)
							continue;
						const float dx = (static_cast<float>(p.gridMin[0] + x) + 0.5f) * p.cellSize - m.from[0];
						const float dy = (static_cast<float>(p.gridMin[1] + y) + 0.5f) * p.cellSize - m.from[1];
						const float w = std::exp(-(dx * dx + dy * dy) / (r * r));
						if (w < 1e-3f)
							continue;
						v.x += std::round(m.velocity[0] * w * spread * 256.0f) / 256.0f;
						v.y += std::round(m.velocity[1] * w * spread * 256.0f) / 256.0f;
					}
					forced[Index(x, y)] = v;
				}
			}

			// Advect
			std::vector<Vec2> advected(Size());
			for (int y = 0; y < n; y++) {
				for (int x = 0; x < n; x++) {
					const Vec2 v = forced[Index(x, y)];
					const float sx = x + 0.5f - p.deltaTime * v.x / p.cellSize;
					const float sy = y + 0.5f - p.deltaTime * v.y / p.cellSize;
					const Vec2 s = SampleBilinear(forced, sx, sy);
					advected[Index(x, y)] = { s.x * p.velocityDecay, s.y * p.velocityDecay };
				}
			}

			// Divergence
			for (int y = 0; y < n; y++) {
				for (int x = 0; x < n; x++) {
					const float left = LoadClamped(advected, x - 1, y).x;
					const float right = LoadClamped(advected, x + 1, y).x;
					const float down = LoadClamped(advected, x, y - 1).y;
					const float up = LoadClamped(advected, x, y + 1).y;
					divergence[Index(x, y)] = (right - left + up - down) / (2.0f * p.cellSize);
				}
			}

			// Jacobi from zero
			std::vector<float> a(Size(), 0.0f), b(Size(), 0.0f);
			for (std::uint32_t i = 0; i < p.jacobiIterations; i++) {
				for (int y = 0; y < n; y++) {
					for (int x = 0; x < n; x++) {
						const float neighbours = LoadOrZero(a, x - 1, y) + LoadOrZero(a, x + 1, y) + LoadOrZero(a, x, y - 1) + LoadOrZero(a, x, y + 1);
						b[Index(x, y)] = (neighbours - p.cellSize * p.cellSize * divergence[Index(x, y)]) * 0.25f;
					}
				}
				std::swap(a, b);
			}
			pressure = a;

			// Project
			for (int y = 0; y < n; y++) {
				for (int x = 0; x < n; x++) {
					const float left = LoadOrZero(pressure, x - 1, y);
					const float right = LoadOrZero(pressure, x + 1, y);
					const float down = LoadOrZero(pressure, x, y - 1);
					const float up = LoadOrZero(pressure, x, y + 1);
					const Vec2 v = advected[Index(x, y)];
					velocity[Index(x, y)] = { v.x - (right - left) / (2.0f * p.cellSize), v.y - (up - down) / (2.0f * p.cellSize) };
				}
			}

			prevGridMin[0] = p.gridMin[0];
			prevGridMin[1] = p.gridMin[1];
			historyValid = true;
		}

		/** @brief Max |divergence| of the current velocity, for the physics sanity check. */
		float MaxDivergenceOfVelocity(float cellSize) const
		{
			float result = 0.0f;
			for (int y = 0; y < n; y++) {
				for (int x = 0; x < n; x++) {
					const float d = (LoadClamped(velocity, x + 1, y).x - LoadClamped(velocity, x - 1, y).x +
										LoadClamped(velocity, x, y + 1).y - LoadClamped(velocity, x, y - 1).y) /
					                (2.0f * cellSize);
					result = std::max(result, std::abs(d));
				}
			}
			return result;
		}

		int n;
		std::vector<Vec2> velocity;
		std::vector<float> divergence;
		std::vector<float> pressure;

	private:
		std::size_t Size() const { return static_cast<std::size_t>(n) * n; }
		std::size_t Index(int x, int y) const { return static_cast<std::size_t>(y) * n + x; }

		Vec2 LoadClamped(const std::vector<Vec2>& f, int x, int y) const
		{
			return f[Index(std::clamp(x, 0, n - 1), std::clamp(y, 0, n - 1))];
		}

		float LoadOrZero(const std::vector<float>& f, int x, int y) const
		{
			return (x < 0 || y < 0 || x >= n || y >= n) ? 0.0f : f[Index(x, y)];
		}

		static constexpr float VORTICITY = 0.6f;  // AddForcesCS's

		/**
		 * @brief The previous step's velocity as the GPU reads it at logical (x, y), clamped to the grid: from the toroidal
		 * texel the cell maps to, so a cell that scrolled in reads whatever the previous window left there.
		 */
		Vec2 PreviousAt(const StepParams& p, const int a_prev[2], int x, int y) const
		{
			x = std::clamp(x, 0, n - 1);
			y = std::clamp(y, 0, n - 1);
			const int px = ((p.gridMin[0] + x - a_prev[0]) % n + n) % n;
			const int py = ((p.gridMin[1] + y - a_prev[1]) % n + n) % n;
			return velocity[Index(px, py)];
		}

		/** @brief AddForcesCS's Curl, on the previous step's velocity. */
		float Curl(const StepParams& p, const int a_prev[2], int x, int y) const
		{
			const Vec2 east = PreviousAt(p, a_prev, x + 1, y), west = PreviousAt(p, a_prev, x - 1, y);
			const Vec2 north = PreviousAt(p, a_prev, x, y + 1), south = PreviousAt(p, a_prev, x, y - 1);
			return ((east.y - west.y) - (north.x - south.x)) / (2.0f * p.cellSize);
		}

		Vec2 SampleBilinear(const std::vector<Vec2>& f, float px, float py) const
		{
			const float fx = px - 0.5f, fy = py - 0.5f;
			const float bx = std::floor(fx), by = std::floor(fy);
			const float tx = fx - bx, ty = fy - by;
			const int ix = static_cast<int>(bx), iy = static_cast<int>(by);
			const Vec2 v00 = LoadClamped(f, ix, iy), v10 = LoadClamped(f, ix + 1, iy);
			const Vec2 v01 = LoadClamped(f, ix, iy + 1), v11 = LoadClamped(f, ix + 1, iy + 1);
			auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
			return { lerp(lerp(v00.x, v10.x, tx), lerp(v01.x, v11.x, tx), ty), lerp(lerp(v00.y, v10.y, tx), lerp(v01.y, v11.y, tx), ty) };
		}

		std::int32_t prevGridMin[2]{};
		bool historyValid = false;
	};

	// ---------------------------------------------------------------------------------------
	// GPU side
	// ---------------------------------------------------------------------------------------

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

	bool CreateWarpDevice(Gpu& gpu)
	{
		const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
		return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, gpu.device.put(), nullptr, gpu.context.put()));
	}

	ID3D11ComputeShader* CompileShader(ID3D11Device* device, const std::vector<std::filesystem::path>& roots, const wchar_t* fileName)
	{
		ShaderInclude include(roots);
		winrt::com_ptr<ID3DBlob> blob, errors;
		const auto path = roots.front() / L"FluidField" / fileName;
		const HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, &include, "main", "cs_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.put(), errors.put());
		if (FAILED(hr)) {
			std::fprintf(stderr, "Compile failed: %ls\n%s\n", path.c_str(), errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
			return nullptr;
		}
		ID3D11ComputeShader* shader = nullptr;
		if (FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader)))
			return nullptr;
		return shader;
	}

	/** @brief Copies a texture to the CPU and unwraps it into logical order. */
	template <class T>
	std::vector<T> ReadLogical(const Gpu& gpu, ID3D11Texture2D* texture, const std::int32_t gridMin[2])
	{
		D3D11_TEXTURE2D_DESC desc{};
		texture->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		winrt::com_ptr<ID3D11Texture2D> staging;
		winrt::check_hresult(gpu.device->CreateTexture2D(&desc, nullptr, staging.put()));
		gpu.context->CopyResource(staging.get(), texture);

		D3D11_MAPPED_SUBRESOURCE mapped{};
		winrt::check_hresult(gpu.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));

		const int n = static_cast<int>(desc.Width);
		std::vector<T> result(static_cast<std::size_t>(n) * n);
		for (int sy = 0; sy < n; sy++) {
			const auto* row = reinterpret_cast<const T*>(static_cast<const std::uint8_t*>(mapped.pData) + sy * mapped.RowPitch);
			for (int sx = 0; sx < n; sx++) {
				const int lx = (sx - gridMin[0]) & (n - 1);
				const int ly = (sy - gridMin[1]) & (n - 1);
				result[static_cast<std::size_t>(ly) * n + lx] = row[sx];
			}
		}
		gpu.context->Unmap(staging.get(), 0);
		return result;
	}

	// ---------------------------------------------------------------------------------------
	// Comparison
	// ---------------------------------------------------------------------------------------

	float Magnitude(float v) { return std::abs(v); }
	float Magnitude(Vec2 v) { return std::max(std::abs(v.x), std::abs(v.y)); }
	float Difference(float a, float b) { return std::abs(a - b); }
	float Difference(Vec2 a, Vec2 b) { return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y)); }

	template <class T>
	bool Compare(const char* caseName, const char* layer, const std::vector<T>& gpu, const std::vector<T>& cpu)
	{
		float scale = 0.0f, worst = 0.0f;
		std::size_t worstIndex = 0;
		for (std::size_t i = 0; i < cpu.size(); i++) {
			scale = std::max(scale, Magnitude(cpu[i]));
			const float d = Difference(gpu[i], cpu[i]);
			if (!(d <= worst)) {  // Also catches NaN
				worst = d;
				worstIndex = i;
			}
		}

		const bool nonTrivial = scale > 1e-6f;
		const float relative = nonTrivial ? worst / scale : worst;
		const bool pass = nonTrivial && relative <= TOLERANCE;
		std::printf("  %-10s max|cpu| %-12.6g max err %-12.6g rel %-12.6g %s\n", layer, scale, worst, relative, pass ? "ok" : "FAIL");
		if (!pass)
			std::printf("  [%s] %s: worst texel %zu%s\n", caseName, layer, worstIndex, nonTrivial ? "" : " (layer is all zero, the case does not exercise it)");
		return pass;
	}

	bool CompareAll(const char* caseName, const Gpu& gpu, const WindSolver& solver, const CpuReference& cpu, const std::int32_t gridMin[2])
	{
		bool ok = true;
		ok &= Compare(caseName, "velocity", ReadLogical<Vec2>(gpu, solver.GetVelocityTexture(), gridMin), cpu.velocity);
		ok &= Compare(caseName, "divergence", ReadLogical<float>(gpu, solver.GetDivergenceTexture(), gridMin), cpu.divergence);
		ok &= Compare(caseName, "pressure", ReadLogical<float>(gpu, solver.GetPressureTexture(), gridMin), cpu.pressure);
		return ok;
	}

	StepParams BaseParams(const std::int32_t gridMin[2])
	{
		StepParams p;
		p.gridMin[0] = gridMin[0];
		p.gridMin[1] = gridMin[1];
		p.cellSize = 16.0f;
		p.deltaTime = 1.0f / 30.0f;
		p.velocityDecay = 0.99f;
		p.jacobiIterations = 20;
		return p;
	}

	/** @brief A point mover at texel (27.3, 35.8) of a grid at gridMin, radius 5 texels (16-unit cells). */
	Mover Puff(const std::int32_t gridMin[2])
	{
		const float x = (static_cast<float>(gridMin[0]) + 27.3f) * 16.0f;
		const float y = (static_cast<float>(gridMin[1]) + 35.8f) * 16.0f;
		return { .from = { x, y }, .to = { x, y }, .radius = 80.0f, .shove = 0.0f, .velocity = { 600.0f, -250.0f }, .carve = 0.0f, .pad = {} };
	}
}

int main(int argc, char** argv)
{
	const std::vector<std::filesystem::path> shaderRoots = argc > 2 ? std::vector<std::filesystem::path>{ argv[1], argv[2] } :
	                                                                  std::vector<std::filesystem::path>{ FLUIDFIELD_SHADER_DIR, FLUIDFIELD_COMMON_SHADER_DIR };

	Gpu gpu;
	if (!CreateWarpDevice(gpu)) {
		std::fprintf(stderr, "Could not create a WARP D3D11 device\n");
		return 2;
	}

	constexpr std::uint32_t N = 64;
	WindSolver solver;
	solver.CreateResources(gpu.device.get(), N, [](ID3D11DeviceChild*, const char*) {});
	if (!solver.CompileShaders([&](const wchar_t* fileName) { return CompileShader(gpu.device.get(), shaderRoots, fileName); })) {
		std::fprintf(stderr, "Shader compilation failed (shader root: %ls)\n", shaderRoots.front().c_str());
		return 2;
	}

	bool ok = true;

	// Case 1: one step from rest with an off-centre puff. GridMin is negative and not a
	// multiple of N, so storage and logical order differ on both axes.
	{
		std::puts("Case 1: single step from rest with one mover");
		const std::int32_t gridMin[2] = { -1000037, 70001 };
		const Mover puff = Puff(gridMin);

		StepParams p = BaseParams(gridMin);
		p.movers = { &puff, 1 };

		solver.Reset(gpu.context.get());
		solver.Step(gpu.context.get(), p);

		CpuReference cpu(N);
		cpu.Step(p);

		ok &= CompareAll("case 1", gpu, solver, cpu, gridMin);
	}

	// Case 2: continue from case 1's state while the grid scrolls by (+5, -3) cells. Checks
	// that carried-over texels keep their world position, that newly exposed texels start at
	// rest, and that a non-zero field advects correctly.
	{
		std::puts("Case 2: scroll by (+5, -3) and step again, no mover");
		const std::int32_t gridMin1[2] = { -1000037, 70001 };
		const std::int32_t gridMin2[2] = { gridMin1[0] + 5, gridMin1[1] - 3 };
		const Mover puff = Puff(gridMin1);

		StepParams p1 = BaseParams(gridMin1);
		p1.movers = { &puff, 1 };
		StepParams p2 = BaseParams(gridMin2);

		solver.Reset(gpu.context.get());
		solver.Step(gpu.context.get(), p1);
		solver.Step(gpu.context.get(), p2);

		CpuReference cpu(N);
		cpu.Step(p1);
		cpu.Step(p2);

		ok &= CompareAll("case 2", gpu, solver, cpu, gridMin2);

		// Physics sanity: projection must clearly reduce divergence. 20 Jacobi iterations only
		// converge the low frequencies partway (about half is removed here), hence the loose bar.
		const float before = *std::ranges::max_element(cpu.divergence, {}, [](float d) { return std::abs(d); });
		const float after = cpu.MaxDivergenceOfVelocity(p2.cellSize);
		const bool reduced = after < 0.75f * std::abs(before);
		std::printf("  projection: max|div| %.6g before, %.6g after %s\n", std::abs(before), after, reduced ? "ok" : "FAIL");
		ok &= reduced;
	}

	// Case 3 (D193): a child level inside a parent. The parent steps with a puff; the child starts from the parent's wind
	// (it has no mover of its own); the parent then absorbs the child: its texels well inside the child take the child's
	// mean over their footprint.
	{
		std::puts("Case 3: child level starts from its parent; parent absorbs the child");
		WindSolver parent;
		parent.CreateResources(gpu.device.get(), N, [](ID3D11DeviceChild*, const char*) {}, "Parent");
		if (!parent.CompileShaders([&](const wchar_t* fileName) { return CompileShader(gpu.device.get(), shaderRoots, fileName); })) {
			std::fprintf(stderr, "Shader compilation failed\n");
			return 2;
		}
		// Parent: 32-unit cells; child: 16-unit cells, centred on the parent's centre
		const std::int32_t parentMin[2] = { -500017, 35001 };
		const std::int32_t childMin[2] = { parentMin[0] * 2 + static_cast<std::int32_t>(N) / 2, parentMin[1] * 2 + static_cast<std::int32_t>(N) / 2 };
		const float cx = (static_cast<float>(parentMin[0]) + N * 0.5f + 1.3f) * 32.0f;
		const float cy = (static_cast<float>(parentMin[1]) + N * 0.5f - 2.2f) * 32.0f;
		const Mover puff{ .from = { cx, cy }, .to = { cx, cy }, .radius = 120.0f, .shove = 0.0f, .velocity = { 500.0f, 300.0f }, .carve = 0.0f, .pad = {} };

		StepParams pp = BaseParams(parentMin);
		pp.cellSize = 32.0f;
		pp.movers = { &puff, 1 };
		parent.Reset(gpu.context.get());
		parent.Step(gpu.context.get(), pp);

		StepParams cp = BaseParams(childMin);
		cp.parent = &parent;
		solver.Reset(gpu.context.get());
		solver.Step(gpu.context.get(), cp);

		const auto parentVelocity = ReadLogical<Vec2>(gpu, parent.GetVelocityTexture(), parentMin);
		const auto childVelocity = ReadLogical<Vec2>(gpu, solver.GetVelocityTexture(), childMin);
		float parentMost = 0.0f, childMost = 0.0f;
		for (const auto& v : parentVelocity)
			parentMost = std::max(parentMost, Magnitude(v));
		for (const auto& v : childVelocity)
			childMost = std::max(childMost, Magnitude(v));
		const bool picked = childMost > 0.25f * parentMost;
		std::printf("  child max|v| %.6g from parent max|v| %.6g %s\n", childMost, parentMost, picked ? "ok" : "FAIL");
		ok &= picked;

		// The child's mean over a parent texel's footprint: four bilinear samples a quarter cell from its centre
		const auto childAt = [&](float a_x, float a_y) {
			const float fx = a_x / 16.0f - static_cast<float>(childMin[0]) - 0.5f;
			const float fy = a_y / 16.0f - static_cast<float>(childMin[1]) - 0.5f;
			const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
			const float tx = fx - static_cast<float>(ix), ty = fy - static_cast<float>(iy);
			const auto at = [&](int x, int y) { return childVelocity[static_cast<std::size_t>(std::clamp(y, 0, static_cast<int>(N) - 1)) * N + std::clamp(x, 0, static_cast<int>(N) - 1)]; };
			const Vec2 a = at(ix, iy), b = at(ix + 1, iy), c = at(ix, iy + 1), d = at(ix + 1, iy + 1);
			return Vec2{ (a.x * (1 - tx) + b.x * tx) * (1 - ty) + (c.x * (1 - tx) + d.x * tx) * ty, (a.y * (1 - tx) + b.y * tx) * (1 - ty) + (c.y * (1 - tx) + d.y * tx) * ty };
		};
		parent.Absorb(gpu.context.get(), solver);
		const auto absorbed = ReadLogical<Vec2>(gpu, parent.GetVelocityTexture(), parentMin);
		float worst = 0.0f;
		const int lx = static_cast<int>(N) / 2 + 1, ly = static_cast<int>(N) / 2 - 2;  // well inside the child (past two edge bands)
		for (int y = ly - 3; y <= ly + 3; y++) {
			for (int x = lx - 3; x <= lx + 3; x++) {
				const float wx = (static_cast<float>(parentMin[0] + x) + 0.5f) * 32.0f, wy = (static_cast<float>(parentMin[1] + y) + 0.5f) * 32.0f;
				Vec2 mean{};
				for (int k = 0; k < 4; k++) {
					const Vec2 v = childAt(wx + ((k & 1) ? 8.0f : -8.0f), wy + ((k & 2) ? 8.0f : -8.0f));
					mean.x += 0.25f * v.x;
					mean.y += 0.25f * v.y;
				}
				worst = std::max(worst, Difference(absorbed[static_cast<std::size_t>(y) * N + x], mean));
			}
		}
		const bool absorbedOk = worst <= 1e-3f * std::max(childMost, 1.0f);
		std::printf("  absorb: max err %.6g against the child's mean %s\n", worst, absorbedOk ? "ok" : "FAIL");
		ok &= absorbedOk;
	}

	std::puts(ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
