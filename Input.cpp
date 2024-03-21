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
		case WM_KEYUP:
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP:
		case WM_MOUSEMOVE:
		{
			auto tuple = std::make_tuple(hWnd, message, wParam, lParam);
			{ ZoneScoped;
				std::unique_lock<std::mutex> lock(m_queueLock);
				m_messageQueue.push(tuple);
			}
			return true;
		}
		}
		return false;
	}

	void Input::FlushQueue()
	{ ZoneScoped;
		m_tick += 1ull;

		std::unique_lock<std::mutex> lock(m_queueLock);
		while (!m_messageQueue.empty())
		{
			auto& [hWnd, message, wParam, lParam] = m_messageQueue.front();
			switch (message)
			{
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
				break;
			}
			m_messageQueue.pop();
		}
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