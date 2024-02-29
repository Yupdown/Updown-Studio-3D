#pragma once

#include "pch.h"

namespace udsdx
{
	class ResourceObject;
	class ResourceLoader
	{
	protected:
		ID3D12Device* m_device;
		ID3D12GraphicsCommandList* m_commandList;

	public:
		ResourceLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
		~ResourceLoader();

		virtual std::unique_ptr<ResourceObject> Load(std::wstring_view path) = 0;
	};

	class TextureLoader : public ResourceLoader
	{
	public:
		TextureLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);

		std::unique_ptr<ResourceObject> Load(std::wstring_view path) override;
	};

	class ModelLoader : public ResourceLoader
	{
	public:
		ModelLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);

		std::unique_ptr<ResourceObject> Load(std::wstring_view path) override;
	};

	class ShaderLoader : public ResourceLoader
	{
	protected:
		ID3D12RootSignature* m_rootSignature;

	public:
		ShaderLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature);

		std::unique_ptr<ResourceObject> Load(std::wstring_view path) override;
	};

	class Resource
	{
	private:
		std::unordered_map<std::wstring, std::unique_ptr<ResourceObject>> m_resources;

		std::map<std::wstring, std::unique_ptr<ResourceLoader>> m_loaders;
		std::map<std::wstring, std::wstring> m_extensionDictionary;

	public:
		Resource();
		~Resource();

		void Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature);

	private:
		void InitializeLoaders(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature);
		void InitializeExtensionDictionary();

	public:
		template <typename T>
		T* Load(std::wstring_view path);
	};

	template<typename T>
	inline T* Resource::Load(std::wstring_view path)
	{
		auto iter = m_resources.find(path.data());
		if (iter == m_resources.end())
		{
			return nullptr;
		}
		return dynamic_cast<T*>(iter->second.get());
	}
}