#include "pch.h"
#include "resource.h"
#include "texture.h"

namespace udsdx
{
	Resource::Resource()
	{

	}

	Resource::~Resource()
	{

	}

	void Resource::Initialize()
	{
		std::wstring path = L"resource\\";

		InitializeExtensionDictionary();

		for (const auto& directory : std::filesystem::recursive_directory_iterator(path))
		{
			if (!directory.is_regular_file())
			{
				continue;
			}

			std::wstring path = directory.path().wstring();
			std::wstring suffix = directory.path().extension().wstring();
			std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);

			auto iter = m_extensionDictionary.find(suffix);
			if (iter == m_extensionDictionary.end())
			{
				continue;
			}

			std::cout << "registering: " << directory.path().string() << std::endl;
			m_resources.insert(std::make_pair(path, iter->second(path)));
		}
	}
	void Resource::InitializeExtensionDictionary()
	{
		RegisterExtensionType<Texture>(m_extensionDictionary, L".png");
		RegisterExtensionType<Texture>(m_extensionDictionary, L".jpg");
		RegisterExtensionType<Texture>(m_extensionDictionary, L".jpeg");
		RegisterExtensionType<Texture>(m_extensionDictionary, L".bmp");
		RegisterExtensionType<Texture>(m_extensionDictionary, L".tga");
	}
}