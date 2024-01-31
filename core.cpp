#include "pch.h"
#include "core.h"

#include "singleton.h"
#include "resource.h"
#include "input.h"

namespace udsdx
{
	Core::Core()
	{

	}

	Core::~Core()
	{

	}

	void Core::Initialize()
	{
#if _WIN32_WINNT >= 0x0A00 // _WIN32_WINNT_WIN10
		m_roInitialization = std::make_unique<Microsoft::WRL::Wrappers::RoInitializeWrapper>(RO_INIT_MULTITHREADED);
		assert(SUCCEEDED(*m_roInitialization));
#else
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		assert(SUCCEEDED(hr));
#endif

		INSTANCE(Resource)->Initialize();
		INSTANCE(Input)->Initialize();
	}

	void Core::Update()
	{
		// TODO: Add your update logic here

		INSTANCE(Input)->IncreaseTick();
	}
	bool Core::ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		int w, h;

		switch (message)
		{
		case WM_SIZE:
			w = LOWORD(lParam);
			h = HIWORD(lParam);
			break;

		default:
			return INSTANCE(Input)->ProcessMessage(hWnd, message, wParam, lParam);
		}

		return true;
	}
}
