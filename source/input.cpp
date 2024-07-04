#include "pch.h"
#include "Input.h"

namespace udsdx
{
	Input::Input()
	{
		m_mouse = std::make_unique<Mouse>();
	}

	Input::~Input()
	{

	}

	bool Input::ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case WM_MOUSEACTIVATE:
			// When you click to activate the window, we want Mouse to ignore that event.
			return false;
		case WM_ACTIVATE:
		case WM_ACTIVATEAPP:
		case WM_INPUT:
		case WM_MOUSEMOVE:
		case WM_MOUSEHOVER:
			m_mouse->ProcessMessage(message, wParam, lParam);
			return true;
		case WM_KEYDOWN:
			m_keyMap.SetKey(static_cast<int>(wParam), true, m_tick);
			break;
		case WM_KEYUP:
			m_keyMap.SetKey(static_cast<int>(wParam), false, m_tick);
			break;
		case WM_LBUTTONDOWN:
			m_mouseMap.SetKey(MK_LBUTTON, true, m_tick);
			break;
		case WM_LBUTTONUP:
			m_mouseMap.SetKey(MK_LBUTTON, false, m_tick);
			break;
		case WM_RBUTTONDOWN:
			m_mouseMap.SetKey(MK_RBUTTON, true, m_tick);
			break;
		case WM_RBUTTONUP:
			m_mouseMap.SetKey(MK_RBUTTON, false, m_tick);
			break;
		case WM_MBUTTONDOWN:
			m_mouseMap.SetKey(MK_MBUTTON, true, m_tick);
			break;
		case WM_MBUTTONUP:
			m_mouseMap.SetKey(MK_MBUTTON, false, m_tick);
			break;
		case WM_XBUTTONDOWN:
			switch (HIWORD(wParam))
			{
			case XBUTTON1:
				m_mouseMap.SetKey(XBUTTON1, true, m_tick);
				break;
			case XBUTTON2:
				m_mouseMap.SetKey(XBUTTON2, true, m_tick);
				break;
			}
			break;
		case WM_XBUTTONUP:
			switch (HIWORD(wParam))
			{
			case XBUTTON1:
				m_mouseMap.SetKey(XBUTTON1, false, m_tick);
				break;
			case XBUTTON2:
				m_mouseMap.SetKey(XBUTTON2, false, m_tick);
				break;
			}
			break;
		default:
			return false;
		}
		return true;
	}

	void Input::Initialize(HWND hWnd)
	{
		m_mouse->SetWindow(hWnd);
	}

	void Input::Reset()
	{
		m_keyMap.Clear();
		m_mouseMap.Clear();
		m_tick = 1ull;
	}

	void Input::Update()
	{
		m_tick += 1ull;

		auto& state = m_mouse->GetState();
		m_mouseX = state.x;
		m_mouseY = state.y;
	}

	void Input::SetRelativeMouse(bool value)
	{
		m_mouse->SetMode(value ? Mouse::MODE_RELATIVE : Mouse::MODE_ABSOLUTE);
	}

	bool Input::GetKey(int key) const
	{
		return m_keyMap.GetKey(key, m_tick - 1);
	}

	bool Input::GetKeyDown(int key) const
	{
		return m_keyMap.GetKeyDown(key, m_tick - 1);
	}

	bool Input::GetKeyUp(int key) const
	{
		return m_keyMap.GetKeyUp(key, m_tick - 1);
	}

	bool Input::GetMouseButton(int button) const
	{
		return m_mouseMap.GetKey(button, m_tick - 1);
	}

	bool Input::GetMouseButtonDown(int button) const
	{
		return m_mouseMap.GetKeyDown(button, m_tick - 1);
	}

	bool Input::GetMouseButtonUp(int button) const
	{
		return m_mouseMap.GetKeyUp(button, m_tick - 1);
	}

	int Input::GetMouseX() const
	{
		return m_mouseX;
	}

	int Input::GetMouseY() const
	{
		return m_mouseY;
	}
}