#pragma once

#include "pch.h"

namespace udsdx
{
	class ResourceObject;
	class Resource
	{
	private:
		std::unordered_map<std::wstring, std::unique_ptr<ResourceObject>> m_resources;
		std::map<std::wstring, std::function<std::unique_ptr<ResourceObject>(std::wstring_view)>> m_extensionDictionary;

	public:
		Resource();
		~Resource();

		void Initialize();

	private:
		template <typename T> requires std::is_base_of_v<ResourceObject, T>
		void RegisterExtensionType(std::map<std::wstring, std::function<std::unique_ptr<ResourceObject>(std::wstring_view)>>& dictionary, std::wstring suffix)
		{
			auto callback = [](std::wstring_view path) -> std::unique_ptr<ResourceObject> {
				return std::make_unique<T>(path);
				};
			dictionary.insert(std::make_pair(suffix, callback));
		}
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