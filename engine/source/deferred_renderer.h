#pragma once

#include "pch.h"
#include "frame_resource.h"

namespace udsdx
{
	class Scene;
	class Texture;
	class Camera;
	class LightDirectional;
	class ShadowMap;
	class ScreenSpaceAO;
	class PostProcessBloom;
	class MotionBlur;
	class PostProcessTAA;
	class PostProcessOutline;
	class RaytracingRenderer;

	class DeferredRenderer
	{
	public:
		DeferredRenderer(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
		~DeferredRenderer();

		void OnResize(UINT newWidth, UINT newHeight);
		void BuildDescriptors(DescriptorParam& descriptorParam);
		void BuildAllDescriptors(DescriptorParam& descriptorParam);
		void BuildObjectRootSignature();
		void BuildDeferredRootSignature();
		void RebuildDescriptors();
		void BuildResources();
		void BuildSkyboxPipelineState();

	public:
		// Top-level entry point. Constructs every render pass for the scene and writes the final
		// (post-processed) image into renderParam.RenderTargetView (the swap chain back buffer).
		void Render(RenderParam& renderParam, Scene* scene);

	private:
		void PassRender(RenderParam& renderParam, D3D12_GPU_VIRTUAL_ADDRESS cbvGpu, const std::vector<ID3D12PipelineState*>& pipelineStates);
		void PassRenderShadow(RenderParam& renderParam, Scene* scene, Camera* camera, LightDirectional* light);
		void PassRenderSSAO(RenderParam& renderParam, Camera* camera);
		void PassRenderMain(RenderParam& renderParam, Scene* scene, Camera* camera, D3D12_GPU_VIRTUAL_ADDRESS cameraCbv);
		void PassRenderRaytracing(RenderParam& renderParam, Scene* scene, Camera* camera, D3D12_GPU_VIRTUAL_ADDRESS cameraCbv);

	public:
		CD3DX12_GPU_DESCRIPTOR_HANDLE GetGBufferSrv(UINT index) const { return m_gBuffersGpuSrv[index]; }
		CD3DX12_GPU_DESCRIPTOR_HANDLE GetDepthBufferSrv() const { return m_depthBufferGpuSrv; }
		CD3DX12_GPU_DESCRIPTOR_HANDLE GetStencilBufferSrv() const { return m_stencilBufferGpuSrv; }
		CD3DX12_CPU_DESCRIPTOR_HANDLE GetDepthBufferDsv() const { return m_depthBufferCpuDsv; }
		CD3DX12_CPU_DESCRIPTOR_HANDLE GetDepthBufferReadOnlyDsv() const { return m_depthBufferReadOnlyCpuDsv; }
		CD3DX12_CPU_DESCRIPTOR_HANDLE GetRenderTargetRTVView() const { return m_targetViewCpuRtv; }
		CD3DX12_GPU_DESCRIPTOR_HANDLE GetRenderTargetSrv() const { return m_targetViewGpuSrv; }
		ID3D12Resource*				  GetRenderTargetResource() const { return m_targetBuffer.Get(); }
		ID3D12RootSignature*		  GetDeferredRootSignature() const { return m_deferredRootSignature.Get(); }
		ID3D12RootSignature*		  GetObjectRootSignature() const { return m_objectRootSignature.Get(); }

		RenderOptions&	GetRenderOptionsRef() { return m_renderOptions; }
		ShadowMap*		GetShadowMap() const { return m_shadowMap.get(); }
		ScreenSpaceAO*	GetScreenSpaceAO() const { return m_screenSpaceAO.get(); }
		PostProcessBloom* GetPostProcessBloom() const { return m_postProcessBloom.get(); }
		RaytracingRenderer* GetRaytracingRenderer() const { return m_raytracingRenderer.get(); }

		// Single source of truth for "the raytracer replaces the raster path this frame".
		bool IsRaytracingActive() const;

		void SetClearColor(const Color& clearColor) { m_clearColor = clearColor; }
		void SetClearColor(float r, float g, float b) { m_clearColor = Color(r, g, b, 1.0f); }

	public:
		static constexpr UINT NUM_GBUFFERS = 4;

		static constexpr DXGI_FORMAT GBUFFER_FORMATS[NUM_GBUFFERS] = {
			// _SRGB, not plain UNORM: shaders now write linear albedo (textures are decoded by the
			// sampler), and 8 bits of linear would band badly in the darks. The ROP encodes on
			// write and the sampler decodes on read, so the stored bits stay perceptually spaced.
			DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, // RGB: Albedo color (linear values, sRGB-encoded storage)
			DXGI_FORMAT_R10G10B10A2_UNORM, // RGB: View normal vector (UNORM encoded)
			DXGI_FORMAT_R16G16_FLOAT, // RG: Screen-space UV motion vector delta
			DXGI_FORMAT_R8G8_UNORM, // R: Metallic, G: Roughness
		};

		static constexpr float GBUFFER_CLEAR_VALUES[NUM_GBUFFERS][4] = {
			{ 0.0f, 0.0f, 0.0f, 0.0f },
			{ 0.0f, 0.0f, 0.0f, 0.0f },
			{ 0.0f, 0.0f, 0.0f, 0.0f },
			// Roughness clears to 1, not 0: a zeroed row would clear the whole buffer to a perfect
			// mirror. Matters only once something samples this target, but the value should not be
			// wrong in the meantime.
			{ 0.0f, 1.0f, 0.0f, 0.0f },
		};

		static constexpr DXGI_FORMAT DEPTH_FORMAT = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

	private:
		ID3D12Device* m_device;
		ID3D12GraphicsCommandList* m_commandList;

		UINT m_width = 0;
		UINT m_height = 0;

		Color m_clearColor = Color(0.0f, 0.0f, 0.0f, 1.0f);
		RenderOptions m_renderOptions;

		ComPtr<ID3D12RootSignature> m_objectRootSignature;
		ComPtr<ID3D12RootSignature> m_deferredRootSignature;
		ComPtr<ID3D12RootSignature> m_skyboxRootSignature;
		ComPtr<ID3D12PipelineState> m_skyboxPipelineState;
		ComPtr<ID3D12PipelineState> m_skyboxVelocityPipelineState;

		// Render passes owned by the renderer.
		std::unique_ptr<ShadowMap> m_shadowMap;
		std::unique_ptr<ScreenSpaceAO> m_screenSpaceAO;
		std::unique_ptr<PostProcessBloom> m_postProcessBloom;
		std::unique_ptr<MotionBlur> m_motionBlur;
		std::unique_ptr<PostProcessTAA> m_postProcessTAA;
		std::unique_ptr<PostProcessOutline> m_postProcessOutline;
		// Only constructed when the device reports DXR 1.0 support.
		std::unique_ptr<RaytracingRenderer> m_raytracingRenderer;

		// Multiple Render Target (MRT) for deferred rendering
		std::array<ComPtr<ID3D12Resource>, NUM_GBUFFERS> m_gBuffers;
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, NUM_GBUFFERS> m_gBuffersCpuSrv;
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, NUM_GBUFFERS> m_gBuffersGpuSrv;
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, NUM_GBUFFERS> m_gBuffersCpuRtv;

		// Intermediate target (R11G11B10_FLOAT) and the single depth buffer.
		ComPtr<ID3D12Resource> m_targetBuffer;
		ComPtr<ID3D12Resource> m_depthBuffer;

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_targetViewCpuRtv;
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_targetViewCpuSrv;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_targetViewGpuSrv;

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_depthBufferCpuSrv;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_depthBufferGpuSrv;
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_stencilBufferCpuSrv;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_stencilBufferGpuSrv;
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_depthBufferCpuDsv;
		// Read-only depth-stencil view: lets the lighting / skybox passes stencil/depth-test against the
		// depth buffer while the same resource is simultaneously bound as an SRV.
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_depthBufferReadOnlyCpuDsv;

		std::array<D3D12_RESOURCE_BARRIER, NUM_GBUFFERS + 1> m_gBufferBeginRenderTransitions;
		std::array<D3D12_RESOURCE_BARRIER, NUM_GBUFFERS + 1> m_gBufferEndRenderTransitions;
	};
}
