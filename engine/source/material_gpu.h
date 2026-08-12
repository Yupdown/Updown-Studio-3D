#pragma once

#include "pch.h"

namespace udsdx
{
	// Sentinel for "this renderer has no material yet". Never reaches a shader: the table is
	// indexed by a root SRV, which does no bounds checking, so every draw resolves to a real slot
	// (DefaultMaterialIndex at worst).
	static constexpr UINT InvalidMaterialIndex = 0xFFFFFFFFu;
	// Resource creates its default material first, so it always occupies slot 0.
	static constexpr UINT DefaultMaterialIndex = 0u;

	static constexpr UINT MaterialFlagAlphaTest = 0x1u;   // glTF alphaMode == MASK
	static constexpr UINT MaterialFlagAlphaBlend = 0x2u;  // glTF alphaMode == BLEND
	// Stored but not enforced: DXR 1.0 has no per-material face culling (RAY_FLAG_CULL_* is
	// per-TraceRay) and the raster path already culls back faces unconditionally. Honouring it in
	// the raster path would need a PSO permutation per material.
	static constexpr UINT MaterialFlagDoubleSided = 0x4u;
	// Escape hatch for normal maps authored with the opposite green-channel convention
	// (OpenGL +Y up vs DirectX +Y down). Whole-texture, so it cannot be detected automatically.
	static constexpr UINT MaterialFlagFlipGreenY = 0x8u;
	// Occlusion shares the metallic-roughness image. Informational; shading is identical either way.
	static constexpr UINT MaterialFlagOrmPacked = 0x10u;

	// glTF 2.0 metallic-roughness core plus KHR_materials_ior and _emissive_strength.
	//
	// Mirrors MaterialData in inc_common.hlsl and inc_raytracing.hlsl -- field order and size must
	// stay in lockstep with BOTH. The two HLSL copies exist because inc_raytracing.hlsl
	// deliberately does not include inc_common.hlsl.
	//
	// Textures are referenced by bindless SRV heap index rather than by pointer. The heap allocator
	// never frees, so an index cannot go stale, which is what lets the GPU-side material carry no
	// lifetime dependency on the CPU object graph at all -- the property a path tracer needs when
	// it samples a material from a hit point with no scene access.
	struct MaterialGpu
	{
		Vector4 BaseColorFactor = Vector4(1.0f, 1.0f, 1.0f, 1.0f); //  0

		Vector3 EmissiveFactor = Vector3(0.0f, 0.0f, 0.0f);        // 16
		float EmissiveStrength = 1.0f;                             // 28

		float MetallicFactor = 0.0f;                               // 32
		float RoughnessFactor = 1.0f;                              // 36
		float NormalScale = 1.0f;                                  // 40
		float OcclusionStrength = 1.0f;                            // 44

		float AlphaCutoff = 0.5f;                                  // 48
		float Ior = 1.5f;                                          // 52
		UINT Flags = 0u;                                           // 56
		UINT SamplerMode = 2u;                                     // 60

		UINT BaseColorTexIndex = InvalidSrvIndex;                  // 64
		UINT MetalRoughTexIndex = InvalidSrvIndex;                 // 68
		UINT NormalTexIndex = InvalidSrvIndex;                     // 72
		UINT OcclusionTexIndex = InvalidSrvIndex;                  // 76

		UINT EmissiveTexIndex = InvalidSrvIndex;                   // 80
		UINT Pad0 = 0u;                                            // 84
		UINT Pad1 = 0u;                                            // 88
		UINT Pad2 = 0u;                                            // 92
	};
	static_assert(sizeof(MaterialGpu) == 96, "MaterialGpu must match the HLSL StructuredBuffer stride.");
}
