#pragma once

#include "pch.h"

namespace udsdx
{
	class ResourceObject;
	class Resource
	{
	private:
		std::unordered_map<std::string, std::unique_ptr<ResourceObject>> m_resources;

	public:
		Resource();
		~Resource();

		void Initialize();

	public:
		template <typename T>
		T* Load(std::string_view path);
	};

	template<typename T>
	inline T* Resource::Load(std::string_view path)
	{
		auto iter = m_resources.find(path.data());
		if (iter == m_resources.end())
		{
			return nullptr;
		}
		return dynamic_cast<T*>(iter->second.get());
	}
}