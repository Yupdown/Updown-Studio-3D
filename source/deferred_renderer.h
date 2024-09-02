#pragma once

#include "pch.h"

namespace udsdx
{
	class DeferredRenderer
	{
	public:
		DeferredRenderer(ID3D12Device* device);
		~DeferredRenderer();

		void OnResize(UINT newWidth, UINT newHeight);
		void BuildDescriptors(DescriptorParam& descriptorParam);
		void RebuildDescriptors();
		void BuildResources();

	public:
		void ClearRenderTargets(ID3D12GraphicsCommandList* commandList);
		void SetRenderTargets(ID3D12GraphicsCommandList* commandList);
		void Pass(RenderParam& renderParam);

	private:
		ID3D12Device* m_device;

		UINT m_width = 0;
		UINT m_height = 0;

		static constexpr UINT NUM_GBUFFERS = 3;

		// Multiple Render Target (MRT) for deferred rendering
		std::array<ComPtr<ID3D12Resource>, NUM_GBUFFERS> m_gBuffers;
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, NUM_GBUFFERS> m_gBuffersCpuSrv;
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, NUM_GBUFFERS> m_gBuffersGpuSrv;
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, NUM_GBUFFERS> m_gBuffersCpuRtv;

		// Depth buffer
		ComPtr<ID3D12Resource> m_depthBuffer;
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_depthBufferCpuSrv;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_depthBufferGpuSrv;
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_depthBufferCpuDsv;

		static constexpr DXGI_FORMAT GBUFFER_FORMATS[NUM_GBUFFERS] = {
			DXGI_FORMAT_R8G8B8A8_UNORM,
			DXGI_FORMAT_R16G16_UNORM,
			DXGI_FORMAT_R8G8B8A8_UNORM
		};

		static constexpr DXGI_FORMAT DEPTH_FORMAT = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	};
}