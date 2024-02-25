#pragma once

#include "pch.h"

namespace udsdx
{
	class ResourceObject
	{
	public:
		ResourceObject(std::wstring_view path);
		virtual ~ResourceObject();
	};
}