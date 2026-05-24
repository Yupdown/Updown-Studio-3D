#pragma once

#include "pch.h"
#include "component.h"

namespace udsdx
{
	class Texture;
	class Scene;

	class EnvironmentMap : public Component
	{
	public:
		EnvironmentMap();
		~EnvironmentMap() override;

		void OnInitialize() override;
		void PostUpdate(const Time& time, Scene& scene) override;

		void SetEnvironmentMap(Texture* texture);
		void SetEnvironmentMap(std::wstring_view path);

		Texture* GetSourceTexture() const { return m_sourceTexture; }
		CD3DX12_GPU_DESCRIPTOR_HANDLE GetCubeMapSrvGpu() const { return m_cubeMapGpuSrv; }
		bool HasValidCubeMap() const;

	private:
		void BuildRootSignature();
		void BuildPipelineState();
		void CreateCubeMapResources(UINT faceSize);
		void BuildDescriptors();
		void GenerateCubeMap();

	private:
		static constexpr UINT kFaceCount = 6;

		ID3D12Device* m_device = nullptr;
		Texture* m_sourceTexture = nullptr;

		UINT m_cubeSize = 0;
		UINT m_mipLevels = 0;

		ComPtr<ID3D12RootSignature> m_generationRootSignature;
		ComPtr<ID3D12PipelineState> m_generationPso;
		ComPtr<ID3D12Resource> m_cubeMapResource;

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_cubeMapCpuSrv{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_cubeMapGpuSrv{};
		std::vector<CD3DX12_CPU_DESCRIPTOR_HANDLE> m_cubeMapCpuUavs;
		std::vector<CD3DX12_GPU_DESCRIPTOR_HANDLE> m_cubeMapGpuUavs;
	};
}
