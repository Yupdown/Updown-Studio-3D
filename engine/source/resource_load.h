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

		virtual std::unique_ptr<ResourceObject> Load(std::wstring_view path, const std::type_info* requestedType = nullptr) = 0;
	};

	class TextureLoader : public ResourceLoader
	{
	protected:
		ID3D12CommandQueue* m_commandQueue;

	public:
		TextureLoader(ID3D12Device* device, ID3D12CommandQueue* commandQueue, ID3D12GraphicsCommandList* commandList);

		std::unique_ptr<ResourceObject> Load(std::wstring_view path, const std::type_info* requestedType = nullptr) override;
	};

	class ModelLoader : public ResourceLoader
	{
	public:
		ModelLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);

		std::unique_ptr<ResourceObject> Load(std::wstring_view path, const std::type_info* requestedType = nullptr) override;
	};

	class ShaderLoader : public ResourceLoader
	{
	protected:
		ID3D12RootSignature* m_rootSignature;

	public:
		ShaderLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature);

		std::unique_ptr<ResourceObject> Load(std::wstring_view path, const std::type_info* requestedType = nullptr) override;
	};

	class AudioClipLoader : public ResourceLoader
	{
	protected:
		AudioEngine* m_audioEngine;

	public:
		AudioClipLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);

		std::unique_ptr<ResourceObject> Load(std::wstring_view path, const std::type_info* requestedType = nullptr) override;
	};

	class FontLoader : public ResourceLoader
	{
	public:
		FontLoader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);

		std::unique_ptr<ResourceObject> Load(std::wstring_view path, const std::type_info* requestedType = nullptr) override;
	};

	class Resource
	{
	private:
		std::wstring m_resourceRootPath;
		std::unordered_map<std::wstring, std::vector<std::unique_ptr<ResourceObject>>> m_resources;

		std::unordered_map<std::wstring, std::unique_ptr<ResourceLoader>> m_loaders;
		std::unordered_map<std::wstring, std::wstring> m_extensionDictionary;
		std::unordered_set<std::wstring> m_ignoreFiles;

	public:
		Resource();
		~Resource();

		void Initialize(ID3D12Device* device, ID3D12CommandQueue* commandQueue, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature);
		void SetResourceRootPath(std::wstring_view path);

	private:
		void InitializeLoaders(ID3D12Device* device, ID3D12CommandQueue* commandQueue, ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* rootSignature);
		void InitializeExtensionDictionary();
		void InitializeIgnoreFiles();
		// Walks up the directory hierarchy starting from the executable's location and
		// looks for an ancestor that contains the resource root folder. When found, the
		// process working directory is set to that ancestor so the relative resource root
		// resolves regardless of where the executable is launched from.
		void ResolveResourceRootPath();
		static std::filesystem::path GetExecutableDirectory();
		static std::wstring NormalizePath(std::wstring_view path);

	public:
		template <typename T>
		T* Load(std::wstring_view path);
		template <typename T>
		std::vector<T*> LoadAll();
	};

	template<typename T>
	inline T* Resource::Load(std::wstring_view path)
	{
		std::wstring wsPath = NormalizePath(path);
		auto iter = m_resources.find(wsPath);
		if (iter != m_resources.end())
		{
			for (const auto& resource : iter->second)
			{
				if (auto casted = dynamic_cast<T*>(resource.get()))
				{
					return casted;
				}
			}
		}

		std::filesystem::path pathObj(wsPath);
		std::wstring extension = pathObj.extension().wstring();
		std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

		auto extensionIter = m_extensionDictionary.find(extension);
		if (extensionIter == m_extensionDictionary.end())
		{
			return nullptr;
		}

		auto loaderIter = m_loaders.find(extensionIter->second);
		if (loaderIter == m_loaders.end())
		{
			return nullptr;
		}

		auto loaded = loaderIter->second->Load(wsPath, &typeid(T));
		if (!loaded)
		{
			return nullptr;
		}

		T* casted = dynamic_cast<T*>(loaded.get());
		if (!casted)
		{
			return nullptr;
		}

		auto& resources = m_resources[wsPath];
		resources.emplace_back(std::move(loaded));
		return casted;
	}

	template<typename T>
	inline std::vector<T*> Resource::LoadAll()
	{
		std::vector<T*> ret;
		for (auto& resourceList : m_resources)
		{
			for (auto& resource : resourceList.second)
			{
				auto casted = dynamic_cast<T*>(resource.get());
				if (casted != nullptr)
				{
					ret.push_back(casted);
				}
			}
		}
		return ret;
	}
}