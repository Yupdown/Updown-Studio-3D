#include "pch.h"
#include "Input.h"

namespace udsdx
{
	Input::Input()
	{
		m_tick = 0ull;
		m_mouseX = 0;
		m_mouseY = 0;
	}

	Input::~Input()
	{
	}

	void Input::Initialize()
	{

	}

	void Input::IncreaseTick()
	{
		m_tick += 1ull;
	}

	bool Input::ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
			case WM_KEYDOWN:
				m_keyMap[wParam] = std::make_pair(true, m_tick);
				break;
			case WM_KEYUP:
				m_keyMap[wParam] = std::make_pair(false, m_tick);
				break;
			case WM_LBUTTONDOWN:
				m_mouseMap[VK_LBUTTON] = std::make_pair(true, m_tick);
				break;
			case WM_LBUTTONUP:
				m_mouseMap[VK_LBUTTON] = std::make_pair(false, m_tick);
				break;
			case WM_RBUTTONDOWN:
				m_mouseMap[VK_RBUTTON] = std::make_pair(true, m_tick);
				break;
			case WM_RBUTTONUP:
				m_mouseMap[VK_RBUTTON] = std::make_pair(false, m_tick);
				break;
			case WM_MBUTTONDOWN:
				m_mouseMap[VK_MBUTTON] = std::make_pair(true, m_tick);
				break;
			case WM_MBUTTONUP:
				m_mouseMap[VK_MBUTTON] = std::make_pair(false, m_tick);
				break;
			case WM_XBUTTONDOWN:
				switch (HIWORD(wParam))
				{
					case XBUTTON1:
						m_mouseMap[VK_XBUTTON1] = std::make_pair(true, m_tick);
						break;
					case XBUTTON2:
						m_mouseMap[VK_XBUTTON2] = std::make_pair(true, m_tick);
						break;
				}
				break;
			case WM_XBUTTONUP:
				switch (HIWORD(wParam))
				{
					case XBUTTON1:
						m_mouseMap[VK_XBUTTON1] = std::make_pair(false, m_tick);
						break;
					case XBUTTON2:
						m_mouseMap[VK_XBUTTON2] = std::make_pair(false, m_tick);
						break;
				}
				break;
			case WM_MOUSEMOVE:
				m_mouseX = GET_X_LPARAM(lParam);
				m_mouseY = GET_Y_LPARAM(lParam);
				break;
			default:
				return false;
		}

		return true;
	}

	bool Input::GetKey(int key) const
	{
		auto iter = m_keyMap.find(key);
		if (iter == m_keyMap.end())
		{
			return false;
		}
		return (*iter).second.first;
	}

	bool Input::GetKeyDown(int key) const
	{
		auto iter = m_keyMap.find(key);
		if (iter == m_keyMap.end())
		{
			return false;
		}
		return (*iter).second.first && (*iter).second.second == m_tick;
	}

	bool Input::GetKeyUp(int key) const
	{
		auto iter = m_keyMap.find(key);
		if (iter == m_keyMap.end())
		{
			return false;
		}
		return !(*iter).second.first && (*iter).second.second == m_tick;
	}

	bool Input::GetMouseButton(int button) const
	{
		auto iter = m_mouseMap.find(button);
		if (iter == m_mouseMap.end())
		{
			return false;
		}
		return (*iter).second.first;
	}

	bool Input::GetMouseButtonDown(int button) const
	{
		auto iter = m_mouseMap.find(button);
		if (iter == m_mouseMap.end())
		{
			return false;
		}
		return (*iter).second.first && (*iter).second.second == m_tick;
	}

	bool Input::GetMouseButtonUp(int button) const
	{
		auto iter = m_mouseMap.find(button);
		if (iter == m_mouseMap.end())
		{
			return false;
		}
		return !(*iter).second.first && (*iter).second.second == m_tick;
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