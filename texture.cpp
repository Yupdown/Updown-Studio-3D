#include "pch.h"
#include "texture.h"

namespace udsdx
{
	Texture::Texture(std::wstring_view path) : ResourceObject(path)
	{
		ComPtr<ID3D12Resource> resource;
	}

	Texture::~Texture()
	{

	}
}