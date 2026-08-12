#pragma once

#include "pch.h"

namespace udsdx
{
	using Vector2 = DirectX::SimpleMath::Vector2;
	using Vector2Int = XMINT2;
	using Vector3 = DirectX::SimpleMath::Vector3;
	using Vector3Int = XMINT3;
	using Vector4 = DirectX::SimpleMath::Vector4;
	using Vector4Int = XMINT4;
	using Color = DirectX::SimpleMath::Color;
	using Plane = DirectX::SimpleMath::Plane;
	using Quaternion = DirectX::SimpleMath::Quaternion;
	using Matrix4x4 = DirectX::SimpleMath::Matrix;

	struct Time
	{
		float deltaTime;
		float totalTime;
	};

	class Camera;
	class ShadowMap;
	class ScreenSpaceAO;
	class DeferredRenderer;
	class MotionBlur;
	class PostProcessBloom;
	class PostProcessTAA;
	class PostProcessOutline;
	class BoundingCamera;
	class EnvironmentMap;
	class RaytracingRenderer;
	class Streamline;

	// Visualisation modes for the raytracing renderer. Anything other than None bypasses the
	// progressive accumulator so the buffer shows the current frame only.
	// Which denoiser consumes the raytracer's output.
	//
	// Off is not just a debug curiosity: it shows the raw per-frame estimate, which is exactly the
	// signal Ray Reconstruction is fed, so it is the honest baseline to judge either denoiser
	// against.
	enum class RaytracingDenoiserMode : UINT
	{
		Off = 0,
		Builtin,
		DlssRayReconstruction,
		Count
	};

	enum class RaytracingDebugMode : UINT
	{
		None = 0,
		Albedo,
		Normal,
		DirectOnly,
		IndirectOnly,
		MotionVector,
		SampleHeatmap,
		Count
	};

	struct RenderOptions
	{
		bool DrawSSAO = true;
		bool DrawMotionBlur = true;
		bool DrawBloom = true;
		bool DrawTAA = true;
		bool DrawOutline = true;
		bool DrawShadowMap = true;
		unsigned int ShadowMapSize = 2048u;
		float MotionBlurShutterSpeed = 0.01f;
		Color FogColor = Color(1.381f, 1.691f, 2.000f, 1.0f);
		Color FogSunColor = Color(2.000f, 1.433f, 0.987f, 1.0f);
		float FogDensity = 0.03f;
		float FogHeightFalloff = 0.2f;
		float FogDistanceStart = 20.0f;

		// Raytracing renderer. When DrawRaytracing is set the whole raster path (G-buffer, SSAO,
		// deferred lighting, forward, TAA, motion blur, outline) is replaced by a full-screen DXR
		// pass; only the bloom pass still runs, because it owns tonemapping and the back-buffer write.
		bool DrawRaytracing = false;
		RaytracingDenoiserMode RaytracingDenoiser = RaytracingDenoiserMode::Builtin;
		// Height of the raytracer's internal buffers. 0 means render at the display resolution,
		// which is what DLAA wants; anything smaller is reconstructed up to the display size by
		// DLSS Ray Reconstruction. The other two denoisers have no reconstruction of their own, so
		// for them a smaller buffer is simply a cheaper image stretched at the resolve.
		unsigned int RaytracingRenderHeight = 0u;
		unsigned int RaytracingSamplesPerPixel = 1u;
		// Temporal reprojection: history is capped at MaxSamplesMoving while a pixel is in motion
		// and allowed up to MaxSamplesStatic once it reprojects exactly.
		float RaytracingMaxSamplesMoving = 32.0f;
		float RaytracingMaxSamplesStatic = 1024.0f;
		float RaytracingVarianceClipGamma = 2.0f;
		float RaytracingNormalThreshold = 0.9f;
		float RaytracingDepthThreshold = 0.05f;
		// Equidistant (r = f * theta) fisheye, full-frame: the image circle is fit to the screen
		// diagonal and the sides are cropped, so the corners see exactly the configured field.
		// Raytracing only -- the raster path has a projection matrix and cannot represent this.
		bool RaytracingFisheye = false;
		float RaytracingFisheyeFov = 180.0f; // degrees, total field; 180 puts the corners at 90 off-axis
		float RaytracingSunAngularDiameter = 0.53f;  // degrees; the real sun subtends ~0.53
		float RaytracingRayMaxDistance = 1000.0f;
		// Upper bound on sky radiance sampled by the indirect bounce. The environment map's
		// brightest texels sit orders of magnitude above a shadowed surface's mean, so a single
		// unlucky bounce sample dominates the pixel for hundreds of frames -- the classic firefly.
		// Clamping the tail at the source costs a slight darkening of sky-lit shadow but cuts the
		// indirect estimator's variance enough for dark regions to actually converge.
		float RaytracingSkyMaxRadiance = 8.0f;
		// Edge-aware a-trous passes over the accumulated indirect radiance. Display-side only, so
		// changing them never invalidates history. 0 disables the filter.
		unsigned int RaytracingAtrousIterations = 4u;
		float RaytracingAtrousLuminanceSigma = 1.0f;
		float RaytracingShadowRayOffset = 1e-3f;
		RaytracingDebugMode RaytracingDebug = RaytracingDebugMode::None;
	};

	// Number of cascaded shadow map levels; must match NUM_CASCADES in inc_common.hlsl.
	static constexpr UINT NumShadowCascades = 4;

	struct RenderParam
	{
		ID3D12Device* Device;
		ID3D12GraphicsCommandList2* CommandList;
		ID3D12RootSignature* RootSignature;
		ID3D12DescriptorHeap* SRVDescriptorHeap;

		DeferredRenderer* Renderer;
		RenderOptions* RenderOptions;

		float AspectRatio;
		int FrameResourceIndex;
		int RenderStageIndex;
		const Time& Time;

		const D3D12_VIEWPORT& Viewport;
		const D3D12_RECT& ScissorRect;

		Camera* TargetCamera;
		BoundingCamera* ViewFrustumWorld;
		bool UseFrustumCulling;

		// Shadow view-instancing: when ShadowCascadeCount > 0, renderers cull against all
		// cascades and set the view instance mask per draw instead of using ViewFrustumWorld.
		std::array<BoundingCamera*, NumShadowCascades> ShadowCascadeBounds{};
		UINT ShadowCascadeCount = 0;

		const D3D12_GPU_VIRTUAL_ADDRESS& ConstantBufferView;
		const D3D12_CPU_DESCRIPTOR_HANDLE& RenderTargetView;

		ID3D12Resource* RenderTargetResource;

		SpriteBatch* SpriteBatchNonPremultipliedAlpha;
		SpriteBatch* SpriteBatchPreMultipliedAlpha;

		ShadowMap* RenderShadowMap;
		ScreenSpaceAO* RenderScreenSpaceAO;
		MotionBlur* RenderMotionBlur;
		PostProcessBloom* RenderPostProcessBloom;
		PostProcessTAA* RenderPostProcessTAA;
		PostProcessOutline* RenderPostProcessOutline;
		EnvironmentMap* RenderEnvironmentMap;

		TracyD3D12Ctx* TracyQueueContext;

		// Appended last: RenderParam is aggregate-initialised with designated initialisers in
		// Core::Render, so inserting fields earlier would break that initialiser's ordering.
		ID3D12GraphicsCommandList4* DXRCommandList = nullptr;
		bool RaytracingSupported = false;
		RaytracingRenderer* RenderRaytracing = nullptr;
		// Null unless Streamline loaded. The raytracing renderer asks it whether Ray
		// Reconstruction can run before committing to that path.
		Streamline* StreamlineRuntime = nullptr;
		// True when the raytracer replaces the raster path this frame. Distinct from
		// RenderOptions::DrawRaytracing, which is the user's request rather than the resolved state.
		bool RaytracingActive = false;
	};

	// Result of a shader-visible SRV heap allocation. HeapIndex is the absolute index used as the
	// bindless lookup index by shaders, or InvalidSrvIndex when the heap could not satisfy it.
	struct SrvAllocation
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE CpuHandle{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE GpuHandle{};
		UINT HeapIndex = 0xFFFFFFFFu;
	};

	struct DescriptorParam
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE CbvCpuHandle;
		CD3DX12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle;
		CD3DX12_CPU_DESCRIPTOR_HANDLE RtvCpuHandle;
		CD3DX12_CPU_DESCRIPTOR_HANDLE DsvCpuHandle;

		CD3DX12_GPU_DESCRIPTOR_HANDLE CbvGpuHandle;
		CD3DX12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle;

		// GPU handle for the first descriptor of the SRV heap. Lets a Texture compute its own
		// absolute heap index from the current SrvGpuHandle, regardless of how many descriptors
		// other passes advanced past beforehand.
		CD3DX12_GPU_DESCRIPTOR_HANDLE SrvHeapStart;

		UINT CbvSrvUavDescriptorSize;
		UINT RtvDescriptorSize;
		UINT DsvDescriptorSize;
	};

	enum RootParam : UINT
	{
		PerObjectCBV,
		PerMaterialCBV,
		PerCameraCBV,
		BonesCBV,
		PrevBonesCBV,
		PerShadowCBV,
		PerFrameCBV,
		SrcTexTable,
	};

	enum RenderGroup : UINT
	{
		Forward = 0,
		Deferred = 1
	};

	static constexpr int FrameResourceCount = 2;
	static constexpr int SwapChainBufferCount = 2;

	// Sentinel for a texture without a registered SRV (no bindless heap index assigned).
	static constexpr UINT InvalidSrvIndex = 0xFFFFFFFFu;
}