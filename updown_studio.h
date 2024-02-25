#pragma once

#include "pch.h"

namespace udsdx
{
	class UpdownStudio
	{
	public:
		static void Initialize(HINSTANCE hInstance, HWND hWnd);
		static void Update();
		static LRESULT CALLBACK ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	};
}