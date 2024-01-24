#pragma once

#include "pch.h"

namespace udsdx
{
	class UpdownStudio
	{
	public:
		UpdownStudio();
		~UpdownStudio();

	public:
		static void Initialize();
		static void Update();
		static bool ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	};
}