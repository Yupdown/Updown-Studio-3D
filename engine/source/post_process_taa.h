#pragma once

#include "pch.h"

namespace udsdx
{
	class PostProcessTAA
	{
	public:
		PostProcessTAA(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);
		PostProcessTAA(const PostProcessTAA& rhs) = delete;
		PostProcessTAA& operator=(const PostProcessTAA& rhs) = delete;
		~PostProcessTAA() = default;

		void Pass(RenderParam& param);

		void OnResize(UINT newWidth, UINT newHeight);
		void BuildResources();
		void BuildDescriptors(DescriptorParam& descriptorParam);
		void RebuildDescriptors();
		void BuildRootSignature();
		void BuildPipelineState();

	private:
		ID3D12Device* m_device = nullptr;

		UINT m_width = 0;
		UINT m_height = 0;

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_sourceCpuSrv;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_sourceGpuSrv;

		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_historyCpuSrv;
		std::array<CD3DX12_GPU_DESCRIPTOR_HANDLE, 2> m_historyGpuSrv;
		std::array<CD3DX12_CPU_DESCRIPTOR_HANDLE, 2> m_historyCpuRtv;

		ComPtr<ID3D12Resource> m_sourceBuffer;
		std::array<ComPtr<ID3D12Resource>, 2> m_historyBuffers;

		int m_historyReadIndex = 0;
		int m_historyWriteIndex = 1;
		bool m_historyValid = false;
		const Camera* m_lastCamera = nullptr;

		ComPtr<ID3D12RootSignature> m_rootSignature;
		ComPtr<ID3D12PipelineState> m_pso;
	};
}
