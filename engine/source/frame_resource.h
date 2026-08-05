#pragma once

#include "pch.h"

namespace udsdx
{
	struct ObjectConstants
	{
		Matrix4x4 World = Matrix4x4::Identity;
		Matrix4x4 PrevWorld = Matrix4x4::Identity;
	};

	// Per-material root constants (passed directly in the root signature, not a CBV).
	struct MaterialConstants
	{
		UINT SamplerMode = 2;
		UINT MainTexIndex = 0; // bindless SRV heap index of the main texture
	};

	struct CameraConstants
	{
		Matrix4x4 View = Matrix4x4::Identity;
		Matrix4x4 Proj = Matrix4x4::Identity;
		Matrix4x4 ViewProj = Matrix4x4::Identity;
		Matrix4x4 ViewInverse = Matrix4x4::Identity;
		Matrix4x4 ProjInverse = Matrix4x4::Identity;
		Matrix4x4 ViewProjInverse = Matrix4x4::Identity;
		Matrix4x4 PrevViewProj = Matrix4x4::Identity;
		Vector4 CameraPosition = Vector4::Zero;
		Vector2 RenderTargetSize = Vector2::Zero;
		Vector2 ClipOffset = Vector2::Zero;     // current-frame TAA jitter (NDC), removed from motion vectors
		Vector2 PrevClipOffset = Vector2::Zero; // previous-frame TAA jitter (NDC)
	};

	struct ShadowConstants
	{
		Matrix4x4 LightViewProj[4];
		Vector4 LightPosition[4];
		float ShadowDistance[4];
		// LightDirection (12B) + LightIntensity (4B) completes one 16-byte register, so LightColor
		// starts on a fresh row and the HLSL mirror in inc_common.hlsl packs identically.
		Vector3 LightDirection = Vector3::Zero;
		float LightIntensity = 2.0f;
		Color LightColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
	};

	struct PassConstants
	{
		float TotalTime = 0.0f;
		float DeltaTime = 0.0f;
		float MotionBlurFactor = 0.0f;
		float MotionBlurRadius = 0.0f;
		Color FogColor = Color();
		Color FogSunColor = Color();
		float FogDensity = 0.0f;
		float FogHeightFalloff = 0.0f;
		float FogDistanceStart = 0.0f;
	};

	// Mirrors cbRaytracing in inc_raytracing.hlsl. Every group below is padded to a 16-byte
	// register boundary so the HLSL packing rules produce the same layout.
	struct RaytracingConstants
	{
		Matrix4x4 ViewProjInverse = Matrix4x4::Identity;
		// Unjittered current and previous view-projection. The ray generation shader projects each
		// primary hit through both to produce a per-pixel motion vector for temporal reprojection.
		Matrix4x4 ViewProj = Matrix4x4::Identity;
		Matrix4x4 PrevViewProj = Matrix4x4::Identity;
		Vector4 CameraPosition = Vector4::Zero;
		Vector2 RenderTargetSize = Vector2::Zero;
		UINT HistoryValid = 0;
		UINT SamplesPerPixel = 1;

		Vector3 SunDirection = Vector3::Zero;
		float SunIntensity = 2.0f;
		Color SunColor = Color(1.0f, 1.0f, 1.0f, 1.0f);

		float SunCosHalfAngle = 1.0f;
		float RayMaxDistance = 1000.0f;
		float ShadowRayOffset = 1e-3f;
		float SkyIntensity = 1.0f;

		float SkyMaxRadiance = 64.0f;
		UINT DebugMode = 0;
		UINT HasEnvironmentMap = 0;
		UINT FrameSeed = 0;

		// Exponential height fog, mirroring the PassConstants fields the raster path uses so both
		// pipelines read the same RenderOptions values and produce the same fog.
		Color FogColor = Color();
		Color FogSunColor = Color();
		float FogDensity = 0.0f;
		float FogHeightFalloff = 0.0f;
		float FogDistanceStart = 0.0f;
		float FogPad = 0.0f;
	};

	// Mirrors cbAccumulate in cs_raytracing_accumulate.hlsl. Drives the temporal reprojection pass
	// that turns this frame's 1spp radiance plus the reprojected history into the running estimate.
	struct RaytracingAccumulateConstants
	{
		Vector2 RenderTargetSize = Vector2::Zero;
		Vector2 InvRenderTargetSize = Vector2::Zero;

		UINT SamplesPerPixel = 1;
		UINT HistoryValid = 0;
		UINT DebugMode = 0;
		float VarianceClipGamma = 2.0f;

		// Effective sample-count ceiling. A pixel that reprojects exactly may keep accumulating up
		// to the static cap; one that is moving is held at the lower cap so it behaves as an
		// exponential moving average and stale reprojection error cannot pile up.
		float MaxSamplesStatic = 1024.0f;
		float MaxSamplesMoving = 32.0f;
		float NormalThreshold = 0.9f;   // minimum dot(normal, prevNormal) to accept history
		float DepthThreshold = 0.05f;   // maximum relative hit-distance difference
	};

	class FrameResource
	{
	public:
		FrameResource(ID3D12Device* device);
		FrameResource(const FrameResource& rhs) = delete;
		FrameResource& operator=(const FrameResource& rhs) = delete;
		~FrameResource();

	public:
		ID3D12CommandAllocator* GetCommandListAllocator() const;
		UploadBuffer<PassConstants>* GetObjectCB() const;

		UINT64 GetFence() const;
		void SetFence(UINT64 fence);

	private:
		ComPtr<ID3D12CommandAllocator> m_commandListAllocator;
		std::unique_ptr<UploadBuffer<PassConstants>> m_objectCB = nullptr;

		UINT64 m_fence = 0;
	};
}