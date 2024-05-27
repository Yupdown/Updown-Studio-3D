#pragma once

#include "pch.h"

namespace udsdx
{
	class Scene;
	class Camera;
	class LightDirectional;

	class ShadowMap
	{
	public:
		ShadowMap(UINT mapWidth, UINT mapHeight, ID3D12Device* device);
		~ShadowMap();

	public:
		void OnResize(UINT newWidth, UINT newHeight, ID3D12Device* device);
		void BuildDescriptors(DescriptorParam& descriptorParam, ID3D12Device* device);
		void BuildPipelineState(ID3D12Device* pDevice, ID3D12RootSignature* pRootSignature);

		void Pass(RenderParam& param, Scene* target, Camera* camera, LightDirectional* light);

	public:
		D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpu() const;

	protected:
		DXGI_FORMAT m_shadowMapFormat = DXGI_FORMAT_R24G8_TYPELESS;

		UINT m_mapWidth;
		UINT m_mapHeight;

		D3D12_VIEWPORT m_viewport;
		D3D12_RECT m_scissorRect;

		D3D12_CPU_DESCRIPTOR_HANDLE m_dsvCpu;
		D3D12_CPU_DESCRIPTOR_HANDLE m_srvCpu;
		D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpu;

		ComPtr<ID3D12Resource> m_shadowMap;
		ComPtr<ID3D12PipelineState> m_shadowPso;
	};
}