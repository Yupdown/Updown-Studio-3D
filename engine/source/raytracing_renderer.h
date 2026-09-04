#pragma once

#include "pch.h"
#include "frame_resource.h"

namespace udsdx
{
	class Scene;
	class Camera;
	class LightDirectional;
	class AccelerationStructure;

	// Standalone full-screen DXR 1.0 path tracer with velocity-based temporal accumulation.
	//
	// It replaces the raster pipeline rather than augmenting it: primary rays are traced, sun
	// visibility is a shadow ray with a cone-sampled direction, and one cosine-weighted diffuse
	// bounce supplies the radiosity term.
	//
	// Each frame emits a 1spp estimate plus a motion vector, then a compute pass reprojects the
	// previous frame's accumulated estimate through that motion vector and folds the new sample
	// in. A surface that stays on screen keeps its history whether the camera or the object moved;
	// only disoccluded or changed pixels restart. The result resolves into the deferred renderer's
	// HDR target so the existing bloom pass can tonemap it.
	class RaytracingRenderer
	{
	public:
		// Everything baked into the accumulated radiance. Editing any of it makes the existing
		// history a mean of a different quantity, so it has to be dropped. Deliberately excludes
		// the camera and the scene: reprojection handles those now instead of resetting.
		//
		// Compared with memcmp, so it must stay a zero-initialised POD with no padding surprises.
		struct RadianceSettings
		{
			Color FogColor{};
			Color FogSunColor{};
			float FogDensity = 0.0f;
			float FogHeightFalloff = 0.0f;
			float FogDistanceStart = 0.0f;
			float RayMaxDistance = 0.0f;
			float SkyMaxRadiance = 0.0f;
			float SpecularSkyMaxRadiance = 0.0f;
			float SpecularFireflyClamp = 0.0f;
			float ShadowRayOffset = 0.0f;
			Vector3 SunDirection{};
			Color SunColor{};
			float SunIntensity = 0.0f;
			float SunAngularDiameter = 0.0f;
			UINT SamplesPerPixel = 0;
			UINT DebugMode = 0;
			UINT HasEnvironmentMap = 0;
			UINT FisheyeEnabled = 0;
			float FisheyeFov = 0.0f;
			// ReSTIR changes the estimator (bias and variance alike), so its knobs restart history.
			UINT RestirEnabled = 0;
			UINT RestirSpatialSamples = 0;
			float RestirSpatialRadius = 0.0f;
			float RestirTemporalMClamp = 0.0f;
			UINT RestirPermute = 0;
		};

		static constexpr DXGI_FORMAT HISTORY_FORMAT = DXGI_FORMAT_R32G32B32A32_FLOAT;
		static constexpr DXGI_FORMAT RADIANCE_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
		// xy is the UV velocity; z carries the hit point's distance from the previous camera, which
		// the accumulation pass compares against the stored previous depth.
		static constexpr DXGI_FORMAT MOTION_FORMAT = DXGI_FORMAT_R16G16_FLOAT;
		// Shapes dictated by DLSS Ray Reconstruction's guide buffer requirements. They are the
		// only copy of each quantity: the engine's own passes read them too.
		static constexpr DXGI_FORMAT NORMAL_ROUGHNESS_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
		static constexpr DXGI_FORMAT LINEAR_DEPTH_FORMAT = DXGI_FORMAT_R32_FLOAT;
		static constexpr DXGI_FORMAT NOISY_COLOR_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
		// Ray Reconstruction writes through a UAV, which the deferred renderer's intermediate
		// target does not have, so the denoised frame lands here first.
		static constexpr DXGI_FORMAT DLSS_OUTPUT_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
		// Instance indices past 2048 would not survive a half float, so the guide stays FP32.
		static constexpr DXGI_FORMAT GUIDE_FORMAT = DXGI_FORMAT_R32G32B32A32_FLOAT;
		static constexpr DXGI_FORMAT ALBEDO_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
		static constexpr DXGI_FORMAT RESOLVE_FORMAT = DXGI_FORMAT_R11G11B10_FLOAT;
		// ReSTIR reservoirs: sample position + weight and visible position in FP32, and one
		// integer texel of packed radiance/normal/counters (see inc_restir.hlsl).
		static constexpr DXGI_FORMAT RESERVOIR_SAMPLE_FORMAT = DXGI_FORMAT_R32G32B32A32_FLOAT;
		static constexpr DXGI_FORMAT RESERVOIR_PACKED_FORMAT = DXGI_FORMAT_R32G32B32A32_UINT;

		RaytracingRenderer(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList);
		RaytracingRenderer(const RaytracingRenderer&) = delete;
		RaytracingRenderer& operator=(const RaytracingRenderer&) = delete;
		~RaytracingRenderer();

		void BuildRootSignatures();
		void BuildStateObject();
		void BuildShaderTables();
		void EnsureHitGroupTable(UINT geometryCount);
		void BuildAccumulatePipelineState();
		void BuildAtrousPipelineState();
		void BuildResolvePipelineState();
		void BuildResources();
		void BuildDescriptors(DescriptorParam& descriptorParam);
		void RebuildDescriptors();
		void OnResize(UINT newWidth, UINT newHeight);
		// Selects the internal buffer height. 0 restores display resolution. Recreates every
		// raytracing buffer, so it is only acted on when the value actually changes.
		void SetRenderHeight(UINT renderHeight);
		UINT GetRequestedRenderHeight() const { return m_requestedRenderHeight; }
		UINT GetRenderWidth() const { return m_renderWidth; }
		UINT GetRenderHeight() const { return m_renderHeight; }
		// The denoiser that actually ran last frame: the requested one falls back to the built-in
		// path when Ray Reconstruction cannot run (no runtime, unsupported, fisheye, debug views).
		RaytracingDenoiserMode GetActiveDenoiser() const { return m_activeDenoiser; }

		// Builds the acceleration structure, dispatches rays, accumulates temporally and resolves
		// into the HDR target.
		void Pass(RenderParam& param, Scene* scene, Camera* camera, LightDirectional* light);

		// False when the state object failed to build or the SRV heap ran dry, in which case the
		// caller falls back to the deferred path instead of tracing an incomplete scene.
		bool IsAvailable() const;

		// Drops the accumulated history. Needed on resize and when re-entering the mode; ordinary
		// camera or scene motion is handled by reprojection and must not call this.
		void InvalidateHistory() { m_historyValid = false; }
		bool IsHistoryValid() const { return m_historyValid; }
		// Restarts the frame counter that seeds the per-pixel RNG and the Halton jitter sequence.
		// With the history also invalidated, a re-convergence then replays the exact same sample
		// sequence -- which is what makes converged images reproducible across runs. Verification
		// use only; never needed for rendering.
		void ResetFrameCounter() { m_frameCounter = 0; }
		// Sources the motion blur pass consumes. Valid after Pass() has run for the frame.
		// The depth SRV is a swizzled view of the guide buffer that presents camera distance in
		// .r, which is where the blur shader looks; it is already linear, so that pass must be
		// told not to linearize it.
		D3D12_GPU_DESCRIPTOR_HANDLE GetMotionSrv() const { return m_motionGpuSrv; }
		D3D12_GPU_DESCRIPTOR_HANDLE GetLinearDepthSrv() const { return m_guideDepthGpuSrv[m_currentGuideIndex]; }

		UINT GetInstanceCount() const;
		UINT GetGeometryCount() const;
		UINT GetBlasCount() const;

	private:
		void CreateDummyEnvironmentCube();
		void UploadConstants(RenderParam& param, Camera* camera, LightDirectional* light, bool hasEnvironmentMap);
		void AccumulateTemporal(RenderParam& param);
		// Runs DLSS Ray Reconstruction in place of the two passes above. Returns false when
		// anything is missing, which leaves the caller to fall back for this frame.
		bool DenoiseWithRayReconstruction(RenderParam& param);
		void AtrousFilter(RenderParam& param);
		void ResolveToTarget(RenderParam& param);
		void TransitionForWrite(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource);
		void TransitionForRead(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource);

		ID3D12Device5* m_device = nullptr;
		ID3D12GraphicsCommandList4* m_commandList = nullptr;

		UINT m_width = 0;
		UINT m_height = 0;

		// Resolution of every raytracing buffer. Equal to the display size unless a render height
		// has been selected, in which case DLSS reconstructs the difference and the other two
		// denoisers are simply stretched by the resolve.
		UINT m_renderWidth = 0;
		UINT m_renderHeight = 0;
		UINT m_requestedRenderHeight = 0;


		std::unique_ptr<AccelerationStructure> m_accelerationStructure;

		ComPtr<ID3D12RootSignature> m_globalRootSignature;
		ComPtr<ID3D12RootSignature> m_hitGroupLocalRootSignature;
		ComPtr<ID3D12StateObject> m_stateObject;
		ComPtr<ID3D12StateObjectProperties> m_stateObjectProperties;
		ComPtr<ID3D12Resource> m_shaderTable;
		ComPtr<ID3D12Resource> m_hitGroupTable;
		UINT m_hitGroupCapacity = 0;
		std::vector<std::pair<ComPtr<ID3D12Resource>, UINT64>> m_retiredHitGroupTables;
		UINT64 m_frameCounter = 0;
		D3D12_DISPATCH_RAYS_DESC m_dispatchDesc = {};

		ComPtr<ID3D12RootSignature> m_accumulateRootSignature;
		ComPtr<ID3D12PipelineState> m_accumulatePipelineState;
		ComPtr<ID3D12RootSignature> m_atrousRootSignature;
		ComPtr<ID3D12PipelineState> m_atrousPipelineState;
		ComPtr<ID3D12RootSignature> m_resolveRootSignature;
		ComPtr<ID3D12PipelineState> m_resolvePipelineState;

		// Ray generation outputs, consumed by the accumulation pass in the same frame. Direct and
		// indirect radiance are separate so the a-trous filter can smooth only the indirect term.
		ComPtr<ID3D12Resource> m_radianceBuffer;
		ComPtr<ID3D12Resource> m_indirectRadianceBuffer;
		// Primary-surface albedo from the centre guide ray. The indirect channel accumulates and
		// filters demodulated irradiance; the resolve multiplies this back in per pixel, so texture
		// detail never passes through the a-trous blur.
		ComPtr<ID3D12Resource> m_albedoBuffer;
		ComPtr<ID3D12Resource> m_motionBuffer;
		// Written for DLSS Ray Reconstruction, and read by the engine's own denoiser wherever the
		// same quantity is needed -- the normal in particular, which used to be octahedral-packed
		// into the guide. Ping-ponged with the guide because history validation compares this
		// frame's normal against the previous frame's.
		std::array<ComPtr<ID3D12Resource>, 2> m_normalRoughnessBuffers;
		// Pure DLSS inputs: nothing in the fallback path reads these.
		ComPtr<ID3D12Resource> m_linearDepthBuffer;
		ComPtr<ID3D12Resource> m_specularAlbedoBuffer;
		ComPtr<ID3D12Resource> m_noisyColorBuffer;
		ComPtr<ID3D12Resource> m_dlssOutputBuffer;
		// Guide and history ping-pong together on the same index: this frame's write becomes next
		// frame's read, and validation compares the current guide against the previous one.
		std::array<ComPtr<ID3D12Resource>, 2> m_guideBuffers;
		std::array<ComPtr<ID3D12Resource>, 2> m_historyBuffers;
		std::array<ComPtr<ID3D12Resource>, 2> m_indirectHistoryBuffers;
		// A-trous ping-pong targets. Display-side only: the temporal feedback always reads the
		// unfiltered indirect history, so filtering never compounds across frames.
		std::array<ComPtr<ID3D12Resource>, 2> m_filterBuffers;

		// ReSTIR GI reservoirs, three textures per set (sample, visible, packed), ping-ponged on the
		// history index. RayGenMain reads last frame's set and writes this frame's -- initial
		// candidates plus temporal reuse -- and the spatial pass only reads it: what it merges from
		// the neighbours is shaded, never stored, so a neighbour's missing coverage cannot compound
		// through the temporal chain.
		std::array<std::array<ComPtr<ID3D12Resource>, 3>, 2> m_reservoirTemporal;
		// One SRV run per (pass, phase): the set that pass reads, then last frame's guide and
		// normal/roughness (used by the temporal validation only). One UAV run per phase.
		std::array<std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2>, 2> m_restirSrvCpu{};
		std::array<std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2>, 2> m_restirSrvTable{};
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_reservoirUavCpu{};
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2> m_reservoirUavTable{};
		// The spatial pass's ray generation record, dispatched with a copy of m_dispatchDesc.
		D3D12_GPU_VIRTUAL_ADDRESS_RANGE m_restirRayGenRecord{};
		// Resolved once per frame: the option, and a debug view that can show the result.
		bool m_restirActive = false;

		// The five ray generation UAVs are allocated consecutively so one descriptor range covers
		// direct radiance, indirect radiance, motion, the write-side guide and the albedo. One run
		// per ping-pong phase, because the guide it points at alternates.
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_raygenUavCpu{};
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2> m_raygenUavTable{};

		// Accumulation output run per phase: direct history[phase], indirect history[phase].
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_accumulateUavCpu{};
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2> m_accumulateUavTable{};

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_motionCpuSrv{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_motionGpuSrv{};
		// Guide views swizzled so camera distance (.z) reads back through .r.
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_guideDepthCpuSrv{};
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2> m_guideDepthGpuSrv{};

		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_historyCpuSrv{};
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2> m_historyGpuSrv{};
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_indirectHistoryCpuSrv{};
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2> m_indirectHistoryGpuSrv{};
		// Plain per-phase guide SRVs for the a-trous passes' edge weights.
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_guideCpuSrv{};
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2> m_guideGpuSrv{};
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_filterCpuSrv{};
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2> m_filterGpuSrv{};
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_filterCpuUav{};
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2> m_filterGpuUav{};
		// Where the resolve reads its indirect term this frame: the last a-trous target, or the raw
		// indirect history when the filter is disabled.
		D3D12_GPU_DESCRIPTOR_HANDLE m_resolveIndirectSrv{};
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_albedoCpuSrv{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_albedoGpuSrv{};
		// The resolve reads whichever of these the selected denoiser produced.
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_noisyColorCpuSrv{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_noisyColorGpuSrv{};
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_dlssOutputCpuSrv{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_dlssOutputGpuSrv{};
		// For the a-trous passes' normal edge stop. The accumulation pass reads its own pair out
		// of the contiguous SRV run below.
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_normalRoughnessCpuSrv{};
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2> m_normalRoughnessGpuSrv{};

		// Contiguous SRV run the accumulation pass binds as one table:
		// radiance, motion, guide[write], guide[read], history[read].
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_accumulateSrvCpu{};
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2> m_accumulateSrvTable{};

		ComPtr<ID3D12Resource> m_dummyEnvironmentCube;
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_dummyEnvironmentCpuSrv{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_dummyEnvironmentGpuSrv{};

		std::array<std::unique_ptr<UploadBuffer<RaytracingConstants>>, FrameResourceCount> m_constantBuffers;
		std::array<std::unique_ptr<UploadBuffer<RaytracingAccumulateConstants>>, FrameResourceCount> m_accumulateConstantBuffers;

		// Previous frame's unjittered view-projection. Kept here rather than read back from Camera:
		// Camera::UpdateConstantBuffer has already overwritten its own copy by the time this pass
		// runs, so it would hand back the current matrix.
		Matrix4x4 m_prevViewProj = Matrix4x4::Identity;
		// The fisheye forward projection needs the view matrix and eye position of the previous
		// frame, which the combined view-projection cannot supply. Staged during UploadConstants
		// and promoted at the end of it so the pair always describes consecutive frames.
		Matrix4x4 m_prevView = Matrix4x4::Identity;
		Matrix4x4 m_pendingView = Matrix4x4::Identity;
		Vector3 m_prevEyePosition = Vector3::Zero;
		Vector3 m_pendingEyePosition = Vector3::Zero;
		const Camera* m_lastCamera = nullptr;
		RadianceSettings m_lastSettings{};

		// Buffers rest here and are raised to UNORDERED_ACCESS only while being written, so the
		// ping-pong swap never has to track which buffer is in which state. The combined read
		// state covers both the accumulation compute pass and the resolve pixel shader, which
		// matters because the resolve reads whichever history slot the swap just produced.
		static constexpr D3D12_RESOURCE_STATES kRestingState =
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

		// This frame's sub-pixel offset, in pixels within [-0.5, 0.5]. Halton(2,3) rather than the
		// shader's RNG: DLSS has to be told exactly where the sample sat.
		DirectX::XMFLOAT2 m_jitterOffset{};

		// Camera state for Streamline, captured untransposed. The HLSL constant buffer stores the
		// transposes of these; Streamline wants them the way the engine holds them.
		Matrix4x4 m_slViewToClip = Matrix4x4::Identity;
		Matrix4x4 m_slClipToView = Matrix4x4::Identity;
		Matrix4x4 m_slClipToPrevClip = Matrix4x4::Identity;
		Matrix4x4 m_slPrevClipToClip = Matrix4x4::Identity;
		Vector3 m_slCameraRight{ 1.0f, 0.0f, 0.0f };
		Vector3 m_slCameraUp{ 0.0f, 1.0f, 0.0f };
		Vector3 m_slCameraForward{ 0.0f, 0.0f, 1.0f };
		Vector3 m_slCameraPosition{};
		float m_slCameraNear = 0.1f;
		float m_slCameraFar = 1000.0f;
		float m_slCameraFovY = 1.0f;
		float m_slCameraAspect = 1.0f;
		// What the resolve should read this frame, and whether the history has to restart.
		RaytracingDenoiserMode m_activeDenoiser = RaytracingDenoiserMode::Builtin;

		int m_historyReadIndex = 0;
		int m_historyWriteIndex = 1;
		// The guide slot written this frame. Recorded before the ping-pong swap so passes running
		// after Pass() can still find it.
		int m_currentGuideIndex = 1;
		bool m_historyValid = false;
		bool m_stateObjectValid = false;
	};
}
