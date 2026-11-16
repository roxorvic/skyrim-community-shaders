// Fluid Field water, pass 1: refresh the cell map, carry state over (new and reset cells start as vanilla water), apply
// emitters, current, level relaxation and the edge blend to vanilla.
// When a cell's bed changes under live water (late map data), the surface is kept and depth recomputed so it makes no
// waves; a cell that changes between wet and dry restarts as calm vanilla water.

#include "FluidField/WaterCommon.hlsli"

struct Emitter
{
	float2 Position;    // Logical texel coordinates
	float Radius;       // Texels, Gaussian falloff
	float DepthRate;    // World units per second added to depth at the centre
	float2 Velocity;    // Velocity the water near the emitter is pulled toward
	float RadialSpeed;  // Speed the water spreads out from the centre, on top of Velocity (waterfalls, D22); negative marks an actor's body
	float FoamRate;     // Foam made per second at the centre (read by the foam pass)
};

Texture2D<float> DepthIn : register(t0);
Texture2D<float2> VelocityIn : register(t1);
Texture2D<float2> CellMapIn : register(t2);
StructuredBuffer<Emitter> Emitters : register(t3);

// A falls' landing (water flows out of it) or brink (water flows into it)
struct FallCurrent
{
	float2 Position;   // World units
	float2 Direction;  // The falls' horizontal direction (unit length), or zero
	float HalfWidth;   // World units
	float Speed;       // World units per second at its edge
	float Level;       // Its water's surface height
	float Kind;        // 1 a landing, -1 a brink
};
StructuredBuffer<FallCurrent> FallCurrents : register(t11);
// Water whose surface is further than this from a falls' water level is another pool or river: left alone
static const float FALL_LEVEL_MATCH = 64.0;
// A river carries about the same water through every cross-section, so it runs slower where it is deep and faster where
// shallow; the flowmap's current is flat, so it is scaled by REFERENCE_DEPTH over vanilla's depth here, within the limits
static const float REFERENCE_DEPTH = 35.0;  // World units (~0.5 m): the depth at which the current runs at its flowmap speed
static const float2 DEPTH_SPEED_LIMITS = float2(0.25, 1.5);
// Under this river speed (world units/s) the water has no flowmap current: the falls drive all of it, unfaded
static const float NO_RIVER_SPEED = 1.0;
// Slowest the falls drive such water anywhere (world units/s): far from them it still visibly flows
static const float MIN_FALLS_SPEED = 30.0;
// The water stands this much higher at a landing and lower at a brink (world units), within FALL_LEVEL_REACH half-widths
static const float FALL_LEVEL_STEP = 2.0;
static const float FALL_LEVEL_REACH = 2.0;
// Within FALL_CHURN_REACH half-widths of a falls, waves and wakes die this much faster per second: a little at a
// landing (its churn), more at a brink (the water speeding over it carries them off)
static const float FALL_CHURN_REACH = 3.0;
static const float LANDING_DAMPING = 1.0;
static const float BRINK_DAMPING = 3.0;
// The water speeds up toward a brink: this much faster at its edge, easing off over BRINK_RUSH_REACH half-widths
static const float BRINK_RUSH = 1.0;
static const float BRINK_RUSH_REACH = 1.5;
// In a river the falls act within this many half-widths, their outward push and pull at this share: some turbulence where
// they meet it, not a counter-current upstream
static const float RIVER_FALL_REACH = 3.0;
static const float RIVER_RADIAL_SHARE = 0.4;
// Per second the falls' own push (D22 emitters) pulls the water's velocity toward it; their current mostly does the work
static const float FALL_PUSH_PULL = 1.5;
// Per second the water inside an actor's body takes its velocity: solid, so the flow parts around it
static const float BODY_PULL = 30.0;

static const float BED_CHANGE_THRESHOLD = 2.0;  // World units
// Still water calms faster: a current carries waves and wakes off downstream, but in a pond they stay where they were made
static const float STILL_SPEED = 60.0;       // World units/s of current under which the water counts as still
static const float STILL_DAMPING = 1.5;      // Extra per-second damping of waves on still water
static const float STILL_LEVEL_RELAX = 2.0;  // Extra per-second pull of its surface back to rest
// Behind an obstacle the flow breaks up and loses the energy the shallow-water equations would hand back as height (they
// have no turbulence): sheltered water slower than the current around it is pulled back to its level this fast, per second
static const float WAKE_RELAX = 4.0;
// Cells upstream (along the current) looked at for the obstacle that shelters a cell
static const int WAKE_LOOK = 4;
// Per second water standing on a bed above vanilla's water (a rock it climbed, a bank it spilled onto) drains back: slowly
// while it runs fast (a sheet going over a rock keeps going), fast once it is still or slow (a film left behind runs off)
static const float DRY_BED_DRAIN = 0.25;
static const float FILM_DRAIN = 3.0;
static const float SHEET_SPEED = 60.0;  // World units/s at which water on such a bed counts as a running sheet
// Water on such a bed stands at most its velocity head v^2 / 2g above the nearby vanilla water, plus this (world units):
// it climbs only as high as its speed carries it
static const float CLIMB_MARGIN = 3.5;

/** @brief 1 where an obstacle lies upstream of the cell within WAKE_LOOK cells along a_downstream (unit), 0 otherwise. */
float Sheltered(int2 logical, float2 a_downstream)
{
	[unroll] for (int k = 1; k <= WAKE_LOOK; k++)
	{
		if (LoadDepth(DepthIn, logical - int2(round(a_downstream * k))) <= WetThreshold)
			return 1.0;
	}
	return 0.0;
}

/**
 * @brief The rest current with its part into nearby walls removed. The pull toward the current knows nothing of
 * obstacles: next to a rock it kept pushing water into the face, piling it far higher than its momentum could.
 */
float2 AlongWalls(int2 logical, float2 a_rest)
{
	float2 toWall = 0.0;
	[unroll] for (int r = 1; r <= 2; r++)
	{
		const float w = r == 1 ? 1.0 : 0.5;
		toWall += w * float2(LoadDepth(DepthIn, logical + int2(r, 0)) <= WetThreshold ? 1.0 : 0.0, 0.0);
		toWall -= w * float2(LoadDepth(DepthIn, logical + int2(-r, 0)) <= WetThreshold ? 1.0 : 0.0, 0.0);
		toWall += w * float2(0.0, LoadDepth(DepthIn, logical + int2(0, r)) <= WetThreshold ? 1.0 : 0.0);
		toWall -= w * float2(0.0, LoadDepth(DepthIn, logical + int2(0, -r)) <= WetThreshold ? 1.0 : 0.0);
	}
	const float len = length(toWall);
	if (len < 1e-3)
		return a_rest;
	const float2 n = toWall / len;
	return a_rest - n * max(dot(a_rest, n), 0.0);
}

RWTexture2D<float> DepthOut : register(u0);
RWTexture2D<float2> VelocityOut : register(u1);
RWTexture2D<float2> CellMapOut : register(u2);

/**
 * @brief The falls' current mixed into the river's. Water without a river current (a pond, or a flowmap-less water type)
 * takes it all, over all of it, unfaded: out of each landing and into each brink like sources and sinks in potential flow
 * (the sim's walls bend it around the rocks), plus each falls' own direction across its width, with its level a little
 * higher at a landing and lower at a brink. In a river, within RIVER_FALL_REACH half-widths: a landing's jet takes FallMix
 * of the current along its width, a weaker push and pull adds some turbulence, and the river speeds up toward a brink.
 * Nothing reaches past FallReach. a_churn is the extra damping near the falls.
 */
float2 MixFallCurrents(float2 a_worldPos, float a_surface, float2 a_river, out float a_level, out float a_churn)
{
	float2 falls = 0.0;  // Ponds: all of it. Rivers: the jets, which take FallMix of the current where they run
	float2 extra = 0.0;  // Rivers: the local push, pull and rush, on top of the river
	float nearest = 0.0;
	a_level = 0.0;
	a_churn = 0.0;
	const bool noRiver = length(a_river) < NO_RIVER_SPEED;
	for (uint i = 0; i < FallCurrentCount; i++) {
		const FallCurrent c = FallCurrents[i];
		if (abs(a_surface - c.Level) > FALL_LEVEL_MATCH)
			continue;
		const float2 d = a_worldPos - c.Position;
		const float r = length(d);
		const float reach = 1.0 - smoothstep(0.5 * FallReach, FallReach, r);
		// A river already knows where its water goes: the falls only act right around them, or they push it back upstream
		const float local = 1.0 - smoothstep(c.HalfWidth, RIVER_FALL_REACH * c.HalfWidth, r);
		const float fade = noRiver ? 1.0 : min(reach, local);
		if (fade <= 0.0)
			continue;
		if (noRiver)
			a_level += c.Kind * FALL_LEVEL_STEP * (1.0 - smoothstep(0.0, FALL_LEVEL_REACH * c.HalfWidth, r));
		const float near = 1.0 - smoothstep(c.HalfWidth, FALL_CHURN_REACH * c.HalfWidth, r);
		a_churn = max(a_churn, near * (c.Kind > 0.0 ? LANDING_DAMPING : BRINK_DAMPING));
		const float rushWeight = exp(-r * r / max(BRINK_RUSH_REACH * BRINK_RUSH_REACH * c.HalfWidth * c.HalfWidth, 1.0));
		const float rush = c.Kind < 0.0 ? 1.0 + BRINK_RUSH * rushWeight : 1.0;
		const float spread = c.HalfWidth / max(r, c.HalfWidth);
		const float2 radial = d / max(r, 1e-3) * c.Kind * c.Speed * rush * spread * spread * (noRiver ? 1.0 : RIVER_RADIAL_SHARE);
		const float jet = exp(-r * r / max(c.HalfWidth * c.HalfWidth, 1.0));
		const float2 along = c.Direction * c.Speed * jet;
		if (noRiver) {
			falls += (radial + along) * fade;
			nearest = max(nearest, fade);
		} else {
			// Toward a river's brink its own current speeds up
			const float2 riverRush = c.Kind < 0.0 ? a_river * BRINK_RUSH * rushWeight : 0.0;
			falls += along * fade;
			extra += (radial + riverRush) * fade;
			nearest = max(nearest, jet * fade);
		}
	}
	if (noRiver) {
		const float speed = length(falls);
		return nearest > 0.0 && speed > 1e-3 ? falls * max(speed, MIN_FALLS_SPEED) / speed : falls;
	}
	return a_river * (1.0 - FallMix * nearest) + FallMix * falls + extra;
}

/** @brief Hash of a lattice point to [-1, 1]. */
float Hash3(int3 p)
{
	uint h = uint(p.x) * 73856093u ^ uint(p.y) * 19349663u ^ uint(p.z) * 83492791u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return float(h & 0xFFFFu) / 32767.5 - 1.0;
}

/** @brief Smooth value noise in 3D (space and time), in [-1, 1]. */
float ValueNoise(float3 p)
{
	int3 i = int3(floor(p));
	float3 f = p - float3(i);
	float3 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(lerp(Hash3(i), Hash3(i + int3(1, 0, 0)), u.x), lerp(Hash3(i + int3(0, 1, 0)), Hash3(i + int3(1, 1, 0)), u.x), u.y),
		lerp(lerp(Hash3(i + int3(0, 0, 1)), Hash3(i + int3(1, 0, 1)), u.x), lerp(Hash3(i + int3(0, 1, 1)), Hash3(i + int3(1, 1, 1)), u.x), u.y), u.z);
}

/** @brief World units across one stirring swirl, and how fast the stirring pattern changes. */
static const float STIR_SCALE = 160.0;
static const float STIR_RATE = 0.35;
/** @brief Stirring acceleration per unit of river speed (1/s), before Turbulence scales it. */
static const float STIR_STRENGTH = 0.3;  // Shown as geometry, 0.9 stirred a funnel into every eddy

/** @brief Rapids: finer, faster swirls where fast water runs over a rough bed (RapidsChaos). */
static const float CHAOS_SCALE = 56.0;
static const float CHAOS_RATE = 1.2;
/** @brief Chaos acceleration per unit of water speed (1/s) at full roughness, before RapidsChaos scales it. */
static const float CHAOS_STRENGTH = 2.5;
/** @brief World units out to where the bed's roughness is measured, and the bed spread (units) that counts as fully rough. */
static const float ROUGH_RADIUS = 48.0;
static const float ROUGH_FULL = 24.0;

/** @brief Curl noise from a slowly changing stream function: swirls water without piling it up, so rivers never settle. */
float2 StirVelocity(float2 worldPos, float scale, float rate)
{
	float3 p = float3(worldPos / scale, SimTime * rate);
	const float e = 0.5;
	float dPsiDx = ValueNoise(p + float3(e, 0, 0)) - ValueNoise(p - float3(e, 0, 0));
	float dPsiDy = ValueNoise(p + float3(0, e, 0)) - ValueNoise(p - float3(0, e, 0));
	// A second, finer octave
	float3 q = p * float3(2.3, 2.3, 1.7) + 17.0;
	dPsiDx += 0.5 * (ValueNoise(q + float3(e, 0, 0)) - ValueNoise(q - float3(e, 0, 0)));
	dPsiDy += 0.5 * (ValueNoise(q + float3(0, e, 0)) - ValueNoise(q - float3(0, e, 0)));
	return float2(dPsiDy, -dPsiDx) / (2.0 * e);
}

[numthreads(FLUIDFIELD_GROUP_SIZE, FLUIDFIELD_GROUP_SIZE, 1)] void main(uint2 id : SV_DispatchThreadID) {
	if (any(id >= GridRes))
		return;

	int2 logical = StorageToLogical(id);
	int2 cell = GridMin + logical;

	float2 worldPos = CellWorldPos(logical);
	float2 map = SampleWaterMap(worldPos);
	float bed = map.x;
	float detailBed;
	if (ObstacleRes > 0 && LoadObstacle(worldPos, detailBed))
		bed = detailBed;
	float vanillaDepth = HasWater(map.y) ? max(map.y - bed, 0.0) : 0.0;
	CellMapOut[id] = float2(bed, vanillaDepth);

	bool vanillaWet = vanillaDepth > WetThreshold;
	// Rivers follow the flowmap; lakes and the sea (water types without the flowmap) stay still
	float2 restVelocity = vanillaWet ? (FlowmapEnabled != 0 ? SampleRiverFlow(worldPos) * SampleRiverFlowScale(worldPos) : Current) : 0.0;
	restVelocity *= clamp(REFERENCE_DEPTH / max(vanillaDepth, 1.0), DEPTH_SPEED_LIMITS.x, DEPTH_SPEED_LIMITS.y);
	// Eddies are stirred in by the river's own speed: a pond the falls drive would churn all over
	const float riverSpeed = length(restVelocity);
	float fallLevel = 0.0, fallChurn = 0.0;
	if (vanillaWet)
		restVelocity = MixFallCurrents(worldPos, map.y, restVelocity, fallLevel, fallChurn);

	// Cells that scrolled in, and every cell after a reset, start as calm vanilla water
	bool kept = HistoryValid != 0 && all(cell >= PrevGridMin) && all(cell < PrevGridMin + int(GridRes));
	float depth = kept ? DepthIn[id] : vanillaDepth;
	float2 velocity = kept ? VelocityIn[id] : restVelocity;

	if (kept) {
		float oldBed = CellMapIn[id].x;
		if (abs(bed - oldBed) > BED_CHANGE_THRESHOLD) {
			bool wasWet = depth > WetThreshold;
			float keptDepth = oldBed + depth - bed;
			if (wasWet && keptDepth > WetThreshold && vanillaWet) {
				depth = keptDepth;
			} else {
				depth = vanillaDepth;
				velocity = restVelocity;
			}
		}
	}

	if (depth > WetThreshold) {
		float2 position = float2(logical) + 0.5;
		for (uint i = 0; i < EmitterCount; i++) {
			Emitter e = Emitters[i];
			float2 d = position - e.Position;
			float w = exp(-dot(d, d) / (e.Radius * e.Radius));
			// Outward from the centre, strongest around its edge (a plunge spreading out)
			if (e.RadialSpeed < 0.0) {
				velocity = lerp(velocity, e.Velocity, smoothstep(0.3, 0.8, w) * saturate(DeltaTime * BODY_PULL));
				continue;
			}
			float distance = length(d);
			float2 radial = e.RadialSpeed > 0.0 ? (d / max(distance, 1e-3)) * e.RadialSpeed * saturate(distance / max(e.Radius * 0.5, 1e-3)) : 0.0;
			// Wakes and splashes take hold fast; the falls' push gently, or it erases every wake near a landing
			velocity = lerp(velocity, e.Velocity + radial, w * saturate(DeltaTime * (e.RadialSpeed > 0.0 ? FALL_PUSH_PULL : 8.0)));
			depth += e.DepthRate * w * DeltaTime;
		}
	}

	if (Vorticity > 0.0 && depth > WetThreshold && kept) {
		// Stirred, not confined: confinement fed every eddy until it dug a funnel to the bed
		const float speed = length(velocity);
		if (riverSpeed > 1.0)
			velocity += StirVelocity(worldPos, STIR_SCALE, STIR_RATE) * riverSpeed * STIR_STRENGTH * Vorticity * DeltaTime;
		// Rapids: fast water over a rough bed churns. Roughness is the bed's spread on a ring around the cell (rocks and
		// ledges, not the river's slope), so a smooth bed at the same speed stays calm.
		if (RapidsChaos > 0.0 && speed > STILL_SPEED) {
			const int r = max(1, int(round(ROUGH_RADIUS / CellSize)));
			const int2 ring[8] = { int2(r, 0), int2(-r, 0), int2(0, r), int2(0, -r), int2(r, r), int2(-r, r), int2(r, -r), int2(-r, -r) };
			float sum = 0.0, sumSq = 0.0;
			[unroll] for (int k = 0; k < 8; k++)
			{
				float b = CellMapIn[LogicalToStorage(clamp(logical + ring[k], 0, int(GridRes) - 1))].x;
				sum += b;
				sumSq += b * b;
			}
			float spread = sqrt(max(sumSq / 8.0 - (sum / 8.0) * (sum / 8.0), 0.0));
			float rough = saturate(spread / ROUGH_FULL);
			velocity += StirVelocity(worldPos, CHAOS_SCALE, CHAOS_RATE) * speed * CHAOS_STRENGTH * RapidsChaos * rough * DeltaTime;
		}
		// Not a per-cell random nudge: that rings shores at the grid frequency
	}

	if (kept)
		restVelocity = AlongWalls(logical, restVelocity);
	velocity = lerp(velocity, restVelocity, saturate(CurrentRelax * DeltaTime));
	float levelDepth = max(vanillaDepth + fallLevel, 0.0);
	const float still = 1.0 - saturate(length(restVelocity) / STILL_SPEED);
	// 2. Water the flow carried onto a bed above vanilla's water is not pulled off it at once (it would never climb)
	float levelRate = vanillaWet ? LevelRelax + STILL_LEVEL_RELAX * still + fallChurn : DRY_BED_DRAIN + FILM_DRAIN * (1.0 - saturate(length(velocity) / SHEET_SPEED));
	// The wake: sheltered water slower than the current loses its head instead of standing up with it. The obstacle's
	// upstream face is not sheltered, so water still climbs it as far as its speed carries it.
	const float restSpeed = length(restVelocity);
	if (kept && vanillaWet && restSpeed > STILL_SPEED) {
		const float slowed = saturate(1.0 - length(velocity) / restSpeed);
		levelRate += WAKE_RELAX * slowed * Sheltered(logical, restVelocity / restSpeed);
	}
	depth = lerp(depth, levelDepth, saturate(levelRate * DeltaTime));

	// Absorbing border: a per-second pull to vanilla growing quadratically toward the edge, so waves fade instead of bouncing
	float edgeWeight = EdgeWeight(logical);
	float edge = saturate(edgeWeight * edgeWeight * EdgeRelax * DeltaTime);
	velocity = lerp(velocity, restVelocity, edge);
	depth = lerp(depth, vanillaDepth, edge);

	// Energy limit near rocks and banks: water piling against them or carried onto them stands no higher above vanilla's
	// level, and dips no lower below it, than the fastest water around can account for (its velocity head v^2 / 2g)
	if (depth > WetThreshold && kept) {
		bool nearWall = false;
		float speedMax = length(velocity);
		[unroll] for (int ny = -3; ny <= 3; ny++)
		{
			[unroll] for (int nx = -3; nx <= 3; nx++)
			{
				nearWall = nearWall || LoadDepth(DepthIn, logical + int2(nx, ny)) <= WetThreshold;
				if (abs(nx) <= 1 && abs(ny) <= 1)
					speedMax = max(speedMax, length(LoadClamped(VelocityIn, logical + int2(nx, ny))));
			}
		}
		float level = vanillaWet ? map.y : NearbyVanillaLevel(worldPos);
		if ((nearWall || !vanillaWet) && HasWater(level)) {
			float head = speedMax * speedMax / (2.0 * Gravity) + CLIMB_MARGIN;
			depth = min(depth, max(level + head - bed, 0.0));
			if (vanillaWet)
				depth = max(depth, level - head - bed);
		}
	}
	// A swirl dips the surface by about v^2 / g at its core; numerical confinement and stirring dug deeper funnels
	if (vanillaWet && depth > WetThreshold) {
		float speed = length(velocity);
		depth = max(depth, levelDepth - speed * speed / Gravity - CLIMB_MARGIN);
	}
	DepthOut[id] = max(depth, 0.0);
	// Only the deviation from the river decays, so damping calms waves, not the current
	VelocityOut[id] = restVelocity + (velocity - restVelocity) * VelocityDecay * exp(-(STILL_DAMPING * still + fallChurn) * DeltaTime);
}
