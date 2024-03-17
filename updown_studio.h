#pragma once

#include "pch.h"

namespace udsdx
{
	class Core;
	class Scene;

	class UpdownStudio
	{
	private:
		static std::wstring m_className;
		static std::wstring m_windowName;

		static HINSTANCE m_hInstance;
		static HWND m_hWnd;

	public:
		static int Initialize(HINSTANCE hInstance);
		static int Run(std::shared_ptr<Scene> initialScene, int nCmdShow = SW_SHOWNORMAL);
		static void Quit();
		static LRESULT CALLBACK ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
		static void RegisterUpdateCallback(std::function<void(const Time&)> callback);
	};
}