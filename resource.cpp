#include "pch.h"
#include "resource.h"

#include "texture.h"

namespace udsdx
{
	template <typename T> requires std::is_base_of_v<ResourceObject, T>
	constexpr void RegisterSuffix(std::map<std::string, std::function<std::unique_ptr<ResourceObject>(std::string_view)>>& dictionary, std::string suffix)
	{
		auto callback = [](std::string_view path) -> std::unique_ptr<ResourceObject> {
			return std::make_unique<T>(path);
		};
		dictionary.insert(std::make_pair(suffix, callback));
	}

	Resource::Resource()
	{

	}

	Resource::~Resource()
	{

	}

	void Resource::Initialize()
	{
		std::string path = "resource\\";

		std::map<std::string, std::function<std::unique_ptr<ResourceObject>(std::string_view)>> suffixDictionary;

		RegisterSuffix<Texture>(suffixDictionary, ".png");
		RegisterSuffix<Texture>(suffixDictionary, ".jpg");
		RegisterSuffix<Texture>(suffixDictionary, ".bmp");

		for (const auto& directory : std::filesystem::recursive_directory_iterator(path))
		{
			if (!directory.is_regular_file())
			{
				continue;
			}

			std::string path = directory.path().string();
			std::string suffix = directory.path().extension().string();
			std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);

			auto iter = suffixDictionary.find(suffix);
			if (iter == suffixDictionary.end())
			{
				continue;
			}

			std::cout << "registering: " << directory.path().string() << std::endl;
			m_resources.insert(std::make_pair(path, iter->second(path)));
		}
	}
}