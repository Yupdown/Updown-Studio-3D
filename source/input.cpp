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
		case WM_ACTIVATE:
		case WM_ACTIVATEAPP:
			if (wParam)
			{
				m_inFocus = true;
				if (m_relativeMouse)
				{
					ShowCursor(FALSE);
					ClipToWindow(hWnd);
				}
			}
			else
			{
				if (m_relativeMouse)
				{
					ClipCursor(nullptr);
				}
				m_inFocus = false;
			}
			return false;
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
		case WM_MOUSEMOVE:
			m_mouseX = GET_X_LPARAM(lParam);
			m_mouseY = GET_Y_LPARAM(lParam);

			if (m_relativeMouse && m_inFocus)
			{
				// Center mouse
				RECT rect = {};
				std::ignore = GetClientRect(hWnd, &rect);
				POINT center = { (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
				std::ignore = ClientToScreen(hWnd, &center);
				SetCursorPos(center.x, center.y);
			}

			if (m_relativeMouse)
			{
				RECT rect = {};
				std::ignore = GetClientRect(hWnd, &rect);
				POINT center = { (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
				m_mouseX -= center.x;
				m_mouseY -= center.y;
			}
			break;
		default:
			return false;
		}
		return true;
	}

	void Input::IncreaseTick()
	{ ZoneScoped;
		m_tick += 1ull;
	}

	void Input::Reset()
	{
		m_keyMap.Clear();
		m_mouseMap.Clear();
		m_tick = 0ull;
	}

	void Input::ClipToWindow(HWND hWnd)
	{
		assert(hWnd != nullptr);

		RECT rect = {};
		std::ignore = GetClientRect(hWnd, &rect);

		POINT ul;
		ul.x = rect.left;
		ul.y = rect.top;

		POINT lr;
		lr.x = rect.right;
		lr.y = rect.bottom;

		std::ignore = MapWindowPoints(hWnd, nullptr, &ul, 1);
		std::ignore = MapWindowPoints(hWnd, nullptr, &lr, 1);

		rect.left = ul.x;
		rect.top = ul.y;

		rect.right = lr.x;
		rect.bottom = lr.y;

		ClipCursor(&rect);
	}

	void Input::SetRelativeMouse(bool value)
	{
		m_relativeMouse = value;
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
		return m_mouseX;
	}

	int Input::GetMouseY() const
	{
		return m_mouseY;
	}
}