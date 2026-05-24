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
		CD3DX12_GPU_DESCRIPTOR_HANDLE GetIrradianceMapSrvGpu() const { return m_irradianceMapGpuSrv; }
		CD3DX12_GPU_DESCRIPTOR_HANDLE GetPrefilterMapSrvGpu() const { return m_prefilterMapGpuSrv; }
		bool HasValidCubeMap() const;
		bool HasValidIblMaps() const;
		void SetIblRadianceCutoff(float cutoff);
		float GetIblRadianceCutoff() const { return m_iblRadianceCutoff; }

	private:
		void BuildRootSignatures();
		void BuildPipelineStates();
		void CreateCubeMapResources(UINT faceSize);
		void CreateIblResources();
		void BuildDescriptors();
		void GenerateMaps();
		void GenerateCubeMap(ID3D12GraphicsCommandList* commandList);
		void GenerateIrradianceMap(ID3D12GraphicsCommandList* commandList);
		void GeneratePrefilterMap(ID3D12GraphicsCommandList* commandList);

	private:
		static constexpr UINT kFaceCount = 6;
		static constexpr UINT kIrradianceFaceSize = 64;
		static constexpr UINT kPrefilterFaceSize = 256;
		static constexpr UINT kIrradianceSampleCount = 64;
		static constexpr UINT kPrefilterSampleCount = 128;
		static constexpr float kDefaultIblRadianceCutoff = 64.0f;

		ID3D12Device* m_device = nullptr;
		Texture* m_sourceTexture = nullptr;

		UINT m_cubeSize = 0;
		UINT m_mipLevels = 0;
		UINT m_prefilterMipLevels = 0;
		float m_iblRadianceCutoff = kDefaultIblRadianceCutoff;

		ComPtr<ID3D12RootSignature> m_equirectGenerationRootSignature;
		ComPtr<ID3D12RootSignature> m_iblGenerationRootSignature;
		ComPtr<ID3D12PipelineState> m_equirectGenerationPso;
		ComPtr<ID3D12PipelineState> m_irradiancePso;
		ComPtr<ID3D12PipelineState> m_prefilterPso;
		ComPtr<ID3D12Resource> m_cubeMapResource;
		ComPtr<ID3D12Resource> m_irradianceMapResource;
		ComPtr<ID3D12Resource> m_prefilterMapResource;

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_cubeMapCpuSrv{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_cubeMapGpuSrv{};
		std::vector<CD3DX12_CPU_DESCRIPTOR_HANDLE> m_cubeMapCpuUavs;
		std::vector<CD3DX12_GPU_DESCRIPTOR_HANDLE> m_cubeMapGpuUavs;

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_irradianceMapCpuSrv{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_irradianceMapGpuSrv{};
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_irradianceMapCpuUav{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_irradianceMapGpuUav{};

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_prefilterMapCpuSrv{};
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_prefilterMapGpuSrv{};
		std::vector<CD3DX12_CPU_DESCRIPTOR_HANDLE> m_prefilterMapCpuUavs;
		std::vector<CD3DX12_GPU_DESCRIPTOR_HANDLE> m_prefilterMapGpuUavs;
	};
}
