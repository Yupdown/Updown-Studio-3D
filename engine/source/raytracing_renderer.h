#pragma once

#include "pch.h"
#include "frame_resource.h"

namespace udsdx
{
	class Scene;
	class Camera;
	class LightDirectional;
	class AccelerationStructure;

	// Standalone full-screen DXR 1.0 path tracer with progressive accumulation.
	//
	// It replaces the raster pipeline rather than augmenting it: primary rays are traced, sun
	// visibility is a shadow ray with a cone-sampled direction, and one cosine-weighted diffuse
	// bounce supplies the radiosity term. Radiance is summed into an FP32 buffer for as long as the
	// camera, scene and lighting hold still, and resolved into the deferred renderer's HDR target
	// so the existing bloom pass can tonemap it.
	class RaytracingRenderer
	{
	public:
		static constexpr DXGI_FORMAT ACCUMULATION_FORMAT = DXGI_FORMAT_R32G32B32A32_FLOAT;
		static constexpr DXGI_FORMAT RESOLVE_FORMAT = DXGI_FORMAT_R11G11B10_FLOAT;

		RaytracingRenderer(ID3D12Device5* device, ID3D12GraphicsCommandList4* commandList);
		RaytracingRenderer(const RaytracingRenderer&) = delete;
		RaytracingRenderer& operator=(const RaytracingRenderer&) = delete;
		~RaytracingRenderer();

		void BuildRootSignatures();
		void BuildStateObject();
		void BuildShaderTables();
		// Grows the hit group table to cover `geometryCount` records. Record i always carries the
		// constant i, so the contents are position-invariant and only ever need extending.
		void EnsureHitGroupTable(UINT geometryCount);
		void BuildResolvePipelineState();
		void BuildResources();
		void BuildDescriptors(DescriptorParam& descriptorParam);
		void RebuildDescriptors();
		void OnResize(UINT newWidth, UINT newHeight);

		// Builds the acceleration structure, dispatches rays and resolves into the HDR target.
		void Pass(RenderParam& param, Scene* scene, Camera* camera, LightDirectional* light);

		// False when the state object failed to build or the SRV heap ran dry, in which case the
		// caller falls back to the deferred path instead of tracing an incomplete scene.
		bool IsAvailable() const;

		// Why the progressive accumulator last restarted. SceneMotion is the common one: any
		// animating object changes the acceleration structure and correctly invalidates a
		// reference render, so a continuously animated scene never converges by design.
		enum class ResetReason
		{
			None,
			Requested,
			SceneMotion,
			ViewOrLighting,
			SceneAndView
		};
		static const char* ToString(ResetReason reason);

		void RequestAccumulationReset() { m_resetRequested = true; }
		UINT GetAccumulatedSamples() const { return m_accumulatedSamples; }
		UINT GetAccumulatedFrames() const { return m_accumulatedFrames; }
		ResetReason GetLastResetReason() const { return m_lastResetReason; }
		UINT GetInstanceCount() const;
		UINT GetGeometryCount() const;
		UINT GetBlasCount() const;

	private:
		void CreateDummyEnvironmentCube();
		UINT64 HashFrame(const RenderParam& param, Camera* camera, LightDirectional* light, UINT64 sceneHash) const;
		void UploadConstants(RenderParam& param, Camera* camera, LightDirectional* light, bool hasEnvironmentMap);
		void ResolveToTarget(RenderParam& param);

		ID3D12Device5* m_device = nullptr;
		ID3D12GraphicsCommandList4* m_commandList = nullptr;

		UINT m_width = 0;
		UINT m_height = 0;

		std::unique_ptr<AccelerationStructure> m_accelerationStructure;

		ComPtr<ID3D12RootSignature> m_globalRootSignature;
		ComPtr<ID3D12RootSignature> m_hitGroupLocalRootSignature;
		ComPtr<ID3D12StateObject> m_stateObject;
		ComPtr<ID3D12StateObjectProperties> m_stateObjectProperties;
		// Ray generation and miss records are fixed; hit group records scale with the geometry
		// count, so they live in their own growable buffer.
		ComPtr<ID3D12Resource> m_shaderTable;
		ComPtr<ID3D12Resource> m_hitGroupTable;
		UINT m_hitGroupCapacity = 0;
		// Superseded hit group tables, retired for a few frames because the GPU may still be
		// reading them when a growth happens.
		std::vector<std::pair<ComPtr<ID3D12Resource>, UINT64>> m_retiredHitGroupTables;
		UINT64 m_frameCounter = 0;
		D3D12_DISPATCH_RAYS_DESC m_dispatchDesc = {};

		ComPtr<ID3D12RootSignature> m_resolveRootSignature;
		ComPtr<ID3D12PipelineState> m_resolvePipelineState;

		ComPtr<ID3D12Resource> m_accumulationBuffer;
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_accumulationCpuUav{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_accumulationGpuUav{};
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_accumulationCpuSrv{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_accumulationGpuSrv{};

		// A root parameter the shader references must always be bound, even in scenes with no
		// EnvironmentMap, so a 1x1x6 cube stands in.
		ComPtr<ID3D12Resource> m_dummyEnvironmentCube;
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_dummyEnvironmentCpuSrv{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_dummyEnvironmentGpuSrv{};

		std::array<std::unique_ptr<UploadBuffer<RaytracingConstants>>, FrameResourceCount> m_constantBuffers;

		UINT64 m_lastHash = 0;
		UINT64 m_lastSceneHash = 0;
		UINT64 m_lastViewHash = 0;
		ResetReason m_lastResetReason = ResetReason::None;
		UINT m_accumulatedSamples = 0;
		UINT m_accumulatedFrames = 0;
		bool m_resetRequested = true;
		bool m_stateObjectValid = false;
	};
}
