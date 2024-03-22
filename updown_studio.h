#pragma once

#include "pch.h"

#include "resource.h"
#include "resource_object.h"
#include "material.h"
#include "mesh.h"
#include "texture.h"
#include "shader.h"
#include "audio_clip.h"

#include "time_measure.h"
#include "input.h"
#include "audio.h"
#include "core.h"

#include "scene.h"
#include "scene_object.h"
#include "transform.h"
#include "component.h"
#include "mesh_renderer.h"
#include "camera.h"

namespace udsdx
{
	class UpdownStudio
	{
	private:
		static std::wstring m_className;
		static std::wstring m_windowName;

		static HINSTANCE m_hInstance;
		static HWND m_hWnd;

		static std::atomic_bool m_running;

	public:
		static int Initialize(HINSTANCE hInstance);
		static int Run(std::shared_ptr<Scene> beginScene, int nCmdShow = SW_SHOWNORMAL);
		static void MainLoop();
		static void Quit();
		static LRESULT CALLBACK ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
		static void RegisterUpdateCallback(std::function<void(const Time&)> callback);
	};
}