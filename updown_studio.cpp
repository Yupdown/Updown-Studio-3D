#include "pch.h"
#include "updown_studio.h"

#include "core.h"

using namespace udsdx;

void UpdownStudio::Initialize(HINSTANCE hInstance, HWND hWnd)
{
	INSTANCE(Core)->Initialize(hInstance, hWnd);
}

void UpdownStudio::Update()
{
	INSTANCE(Core)->Update();
}

LRESULT CALLBACK UpdownStudio::ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        if (!INSTANCE(Core)->ProcessMessage(hWnd, message, wParam, lParam))
        {
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;
    }
    return 0;
}
