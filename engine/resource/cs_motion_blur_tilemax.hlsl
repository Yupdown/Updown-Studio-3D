// The velocity texture is not necessarily the size of the screen: the raytracer traces into
// smaller buffers and lets the upscaler make up the difference. The tile grid stays in DISPLAY
// pixels regardless, because MAX_BLUR_RADIUS is the pixel shader's clamp radius in display space
// and the Guertin et al. construction only holds while a tile is exactly that wide -- so this pass
// maps its display-space footprint into whatever resolution the source happens to be.
cbuffer cbTileMax : register(b0)
{
	uint2 gSourceSize; // velocity texels
	uint2 gTargetSize; // display pixels the tile grid was built for
};

Texture2D gSource : register(t0);
RWTexture2D<float2> gDestination : register(u0);

[numthreads(16, 16, 1)]
void CS(int3 id : SV_DispatchThreadID)
{
	// The dispatch rounds up to whole 16x16 groups, so the tail threads own no tile. Their writes
	// would be discarded anyway; returning saves them MAX_BLUR_RADIUS^2 loads apiece. Must match
	// MotionBlur::BuildResources, which sizes the destination the same way.
	int2 tileCount = int2((gTargetSize + MAX_BLUR_RADIUS - 1) / MAX_BLUR_RADIUS);
	if (any(id.xy >= tileCount))
	{
		return;
	}

	// At most 1 by construction, so consecutive x steps by less than a source texel and the
	// truncation below can never skip one: the tile's footprint stays fully covered.
	float2 sourceScale = float2(gSourceSize) / float2(gTargetSize);
	int2 sourceLimit = int2(gSourceSize) - 1;

	float2 maxSample = 0.0f;
	for (int y = 0; y < MAX_BLUR_RADIUS; ++y)
	{
		for (int x = 0; x < MAX_BLUR_RADIUS; ++x)
		{
			int2 target = id.xy * MAX_BLUR_RADIUS + int2(x, y);
			// Clamp rather than let an out-of-range Load return zero: only the last tile in a row
			// or column reaches past the edge, and a zero there reads as "nothing is moving",
			// which is the one answer that is never true of a partially covered tile.
			int2 source = min(int2(float2(target) * sourceScale), sourceLimit);
			float2 sample = gSource.Load(int3(source, 0)).xy;
			if (dot(sample, sample) > dot(maxSample, maxSample))
			{
				maxSample = sample;
			}
		}
	}
	gDestination[id.xy] = maxSample;
}
