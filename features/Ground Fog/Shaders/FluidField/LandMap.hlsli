// The loaded landscape round the player (GroundTiles' land map): x height, y flags, 32 units a texel, toroidal, valid within
// LAND_REACH cells of its centre cell. The ground where no tile is resident: shared by FogBedCS, GroundFogCS and GroundDebugCS.

static const float LAND_TEXEL = 32.0, LAND_CELL = 4096.0;
static const int LAND_TEXELS = 1024, LAND_REACH = 2;
static const uint LAND_MAY_FORM = 1, LAND_WATER = 8;
static const float LAND_NONE = -1.0e6;

/** Bilinear height at xy, or LAND_NONE off the map; forming, water: its shares fog may form on and that are water. */
float LandHeight(Texture2D<float2> landMap, int2 landCell, float2 xy, out float forming, out float water)
{
	forming = 0.0;
	water = 0.0;
	if (any(abs(int2(floor(xy / LAND_CELL)) - landCell) > LAND_REACH))
		return LAND_NONE;
	const float2 t = xy / LAND_TEXEL - 0.5;
	const int2 base = int2(floor(t));
	const float2 f = t - float2(base);
	float height = 0.0, form = 0.0, wet = 0.0;
	[unroll] for (uint k = 0; k < 4; k++)
	{
		const int2 offset = int2(k & 1, k >> 1);
		const float2 g = landMap.Load(int3((base + offset) & (LAND_TEXELS - 1), 0));
		const float w = (offset.x ? f.x : 1.0 - f.x) * (offset.y ? f.y : 1.0 - f.y);
		if (g.x < LAND_NONE)
			return LAND_NONE;
		height += g.x * w;
		form += ((uint(g.y) & LAND_MAY_FORM) ? 1.0 : 0.0) * w;
		wet += ((uint(g.y) & LAND_WATER) ? 1.0 : 0.0) * w;
	}
	forming = form;
	water = wet;
	return height;
}
