#pragma once

#include "pch.h"

namespace udsdx
{
	class Scene;
	class Camera;

	class ScreenSpaceAO
	{
	public:
		static constexpr int KERNEL_SIZE = 64;
		static constexpr int NOISE_SIZE = 4;

		static constexpr DXGI_FORMAT AO_FORMAT = DXGI_FORMAT_R8_UNORM;
		static constexpr DXGI_FORMAT NORMAL_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;

	public:
		ScreenSpaceAO(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, UINT width, UINT height);
		ScreenSpaceAO(const ScreenSpaceAO& rhs) = delete;
		ScreenSpaceAO& operator=(const ScreenSpaceAO& rhs) = delete;
		~ScreenSpaceAO() = default;

		void PassNormal(RenderParam& param, Scene* target, Camera* camera);
		void PassSSAO(RenderParam& param);

		void OnResize(UINT newWidth, UINT newHeight, ID3D12Device* device);
		void BuildResources();
		void BuildDescriptors(DescriptorParam& descriptorParam, ID3D12Resource* depthStencilBuffer);
		void RebuildDescriptors(ID3D12Resource* depthStencilBuffer);
		void BuildRootSignature(ID3D12Device* pDevice);
		void BuildPipelineState(ID3D12Device* pDevice, ID3D12RootSignature* pRootSignature);

	private:
		ID3D12Device* m_device;

		UINT m_width;
		UINT m_height;

		ComPtr<ID3D12RootSignature> m_ssaoRootSignature;

		ComPtr<ID3D12PipelineState> m_normalPSO;
		ComPtr<ID3D12PipelineState> m_ssaoPSO;

		ComPtr<ID3D12Resource> m_ambientMap;
		ComPtr<ID3D12Resource> m_normalMap;
		
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_ambientMapCpuSrv;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_ambientMapGpuSrv;
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_ambientMapCpuRtv;

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_normalMapCpuSrv;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_normalMapGpuSrv;
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_normalMapCpuRtv;

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_depthMapCpuSrv;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_depthMapGpuSrv;

		D3D12_VIEWPORT m_viewport;
		D3D12_RECT m_scissorRect;
	};
}