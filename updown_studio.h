#pragma once

#include "pch.h"

namespace udsdx
{
	class Core;

	class UpdownStudio
	{
	private:
		static std::wstring m_className;
		static std::wstring m_windowName;

		static HINSTANCE m_hInstance;
		static HWND m_hWnd;

	public:
		static int Initialize(HINSTANCE hInstance);
		static int Run(int nCmdShow = SW_SHOWNORMAL);
		static void Quit();
		static LRESULT CALLBACK ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	};
}