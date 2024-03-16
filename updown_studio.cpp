#include "pch.h"
#include "updown_studio.h"

#include "core.h"

namespace udsdx
{
    std::wstring UpdownStudio::m_className = L"UpdownStudioClass";
    std::wstring UpdownStudio::m_windowName = L"Updown Studio Application";

    HINSTANCE UpdownStudio::m_hInstance = nullptr;
    HWND UpdownStudio::m_hWnd = nullptr;

    int UpdownStudio::Initialize(HINSTANCE hInstance)
    {
        m_hInstance = hInstance;

        WNDCLASSEXW wcex = { 0 };
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = UpdownStudio::ProcessMessage;
        wcex.hInstance = hInstance;
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.lpszClassName = m_className.c_str();
        RegisterClassExW(&wcex);

        m_hWnd = CreateWindowW(m_className.c_str(), m_windowName.c_str(), WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, hInstance, nullptr);
        if (!m_hWnd)
        {
            return -1;
        }

        // Create console window for debug
#ifdef _DEBUG
        if (AllocConsole())
        {
            FILE* fp;
            freopen_s(&fp, "CONOUT$", "w", stdout);
        }
#endif

        INSTANCE(Core)->Initialize(hInstance, m_hWnd);
        return 0;
    }

    int udsdx::UpdownStudio::Run(std::shared_ptr<Scene> initialScene, int nCmdShow)
    {
        if (!m_hWnd)
		{
			return -1;
		}

        INSTANCE(Core)->SetScene(initialScene);

        ShowWindow(m_hWnd, nCmdShow);
        UpdateWindow(m_hWnd);

        MSG message;
        while (GetMessage(&message, nullptr, 0, 0))
        {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }

        return static_cast<int>(message.wParam);
    }

    void UpdownStudio::Quit()
    {
        INSTANCE(Core)->OnDestroy();
    }

    LRESULT CALLBACK UpdownStudio::ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_DESTROY:
            PostQuitMessage(0);
            break;

        case WM_PAINT:
            INSTANCE(Core)->Update();
            INSTANCE(Core)->Draw();
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
}