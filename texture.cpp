#include "pch.h"
#include "texture.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace udsdx
{
	Texture::Texture(std::string_view path) : ResourceObject(path)
	{
		int width, height, channels;
		unsigned char* data = stbi_load(path.data(), &width, &height, &channels, 0);
		if (data == nullptr)
		{
			throw std::exception("Failed to load texture.");
		}
	}

	Texture::~Texture()
	{

	}
}
