#include "pch.h"
#include "updown_studio.h"

#include "core.h"

namespace udsdx
{
    std::wstring UpdownStudio::m_className = L"UpdownStudioClass";
    std::wstring UpdownStudio::m_windowName = L"Updown Studio Application";

    HINSTANCE UpdownStudio::m_hInstance = nullptr;
    HWND UpdownStudio::m_hWnd = nullptr;

    std::atomic_bool UpdownStudio::m_running = false;
    std::thread g_engineThread;

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

    int udsdx::UpdownStudio::Run(std::shared_ptr<Scene> beginScene, int nCmdShow)
    {
        if (!m_hWnd)
		{
			return -1;
		}

        INSTANCE(Core)->SetScene(beginScene);

        m_running = true;

        ShowWindow(m_hWnd, nCmdShow);
        UpdateWindow(m_hWnd);

        g_engineThread = std::thread(UpdownStudio::MainLoop);

        MSG message;
        while (GetMessage(&message, nullptr, 0, 0))
        { ZoneScopedN("Windows32 Main Loop");
            TranslateMessage(&message);
            DispatchMessage(&message);

            if (!m_running)
            {
                PostQuitMessage(0);
                if (g_engineThread.joinable())
                {
                    g_engineThread.join();
                }
            }
        }
        return static_cast<int>(message.wParam);
    }

    void UpdownStudio::MainLoop()
    {
        tracy::SetThreadName("Engine Thread");

        auto core = INSTANCE(Core);
        while (m_running)
        { ZoneScopedNC("Engine Main Loop", 0x27449F);
            core->AcquireNextFrameResource();
            core->Update();
            core->Render();

            FrameMark;
		}
        core->OnDestroy();
	}

    void UpdownStudio::Quit()
    {
        m_running = false;
    }

    LRESULT CALLBACK UpdownStudio::ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (hWnd != m_hWnd)
        {
			return DefWindowProc(hWnd, message, wParam, lParam);
		}

        switch (message)
        {
        case WM_CREATE:
            break;

        case WM_DESTROY:
            m_running = false;
            g_engineThread.join();
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

    void UpdownStudio::RegisterUpdateCallback(std::function<void(const Time&)> callback)
    {
        INSTANCE(Core)->RegisterUpdateCallback(callback);
    }
}