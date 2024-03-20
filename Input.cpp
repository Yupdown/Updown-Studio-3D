#include "pch.h"
#include "Input.h"

namespace udsdx
{
	Input::Input()
	{

	}

	Input::~Input()
	{

	}

	bool Input::ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
			case WM_KEYDOWN:
				m_keyMap.AddMessage(static_cast<int>(wParam), KeyState::Down);
				break;
			case WM_KEYUP:
				m_keyMap.AddMessage(static_cast<int>(wParam), KeyState::Up);
				break;
			case WM_LBUTTONDOWN:
				m_mouseMap.AddMessage(VK_LBUTTON, KeyState::Down);
				break;
			case WM_LBUTTONUP:
				m_mouseMap.AddMessage(VK_LBUTTON, KeyState::Up);
				break;
			case WM_RBUTTONDOWN:
				m_mouseMap.AddMessage(VK_RBUTTON, KeyState::Down);
				break;
			case WM_RBUTTONUP:
				m_mouseMap.AddMessage(VK_RBUTTON, KeyState::Up);
				break;
			case WM_MBUTTONDOWN:
				m_mouseMap.AddMessage(VK_MBUTTON, KeyState::Down);
				break;
			case WM_MBUTTONUP:
				m_mouseMap.AddMessage(VK_MBUTTON, KeyState::Up);
				break;
			case WM_XBUTTONDOWN:
				switch (HIWORD(wParam))
				{
					case XBUTTON1:
						m_mouseMap.AddMessage(VK_XBUTTON1, KeyState::Down);
						break;
					case XBUTTON2:
						m_mouseMap.AddMessage(VK_XBUTTON2, KeyState::Down);
						break;
				}
				break;
			case WM_XBUTTONUP:
				switch (HIWORD(wParam))
				{
					case XBUTTON1:
						m_mouseMap.AddMessage(VK_XBUTTON1, KeyState::Up);
						break;
					case XBUTTON2:
						m_mouseMap.AddMessage(VK_XBUTTON2, KeyState::Up);
						break;
				}
				break;
			case WM_MOUSEMOVE:
				nextMouseParam = static_cast<LONG>(lParam);
				break;
			default:
				return false;
		}

		return true;
	}

	void Input::FlushQueue()
	{
		m_tick += 1ull;

		m_keyMap.FlushQueue(m_tick);
		m_mouseMap.FlushQueue(m_tick);

		currMouseParam = nextMouseParam;
	}

	bool Input::GetKey(int key) const
	{
		return m_keyMap.GetKey(key, m_tick);
	}

	bool Input::GetKeyDown(int key) const
	{
		return m_keyMap.GetKeyDown(key, m_tick);
	}

	bool Input::GetKeyUp(int key) const
	{
		return m_keyMap.GetKeyUp(key, m_tick);
	}

	bool Input::GetMouseButton(int button) const
	{
		return m_mouseMap.GetKey(button, m_tick);
	}

	bool Input::GetMouseButtonDown(int button) const
	{
		return m_mouseMap.GetKeyDown(button, m_tick);
	}

	bool Input::GetMouseButtonUp(int button) const
	{
		return m_mouseMap.GetKeyUp(button, m_tick);
	}

	int Input::GetMouseX() const
	{
		return GET_X_LPARAM(currMouseParam);
	}

	int Input::GetMouseY() const
	{
		return GET_Y_LPARAM(currMouseParam);
	}
}