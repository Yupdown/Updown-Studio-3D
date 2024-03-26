#pragma once

#include "pch.h"

namespace udsdx
{
	class Scene;

	class LightDirectional
	{
	public:
		LightDirectional(UINT mapWidth, UINT mapHeight, ID3D12Device* device);
		~LightDirectional();

	public:
		void OnResize(UINT newWidth, UINT newHeight, ID3D12Device* device);
		void BuildDescriptors(ID3D12Device* device, CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuSrv, CD3DX12_CPU_DESCRIPTOR_HANDLE hCpuDsv, CD3DX12_GPU_DESCRIPTOR_HANDLE hGpuSrv);
		void BuildPipelineState(ID3D12Device* pDevice, ID3D12RootSignature* pRootSignature);
		void RenderShadowMap(RenderParam& param, Scene& scene);
		void UpdateShadowTransform(const Time& time);

		Matrix4x4 GetShadowTransform() const;
		Vector3 GetLightDirection() const;

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

		Vector3 m_lightDirection;
		Matrix4x4 m_shadowTransform;
	};
}