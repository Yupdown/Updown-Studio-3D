#include "pch.h"
#include "Input.h"

template <typename T>
inline void ProcessKeyMessage(std::map<T, std::pair<bool, unsigned long long>>& keyMap, T key, bool state, unsigned long long tick)
{
	auto iter = keyMap.find(static_cast<int>(key));
	if (iter == keyMap.end() || (*iter).second.first != state)
	{
		keyMap[key] = std::make_pair(state, tick);
	}
}

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
				ProcessKeyMessage(m_keyMap, static_cast<int>(wParam), true, m_tick);
				break;
			case WM_KEYUP:
				ProcessKeyMessage(m_keyMap, static_cast<int>(wParam), false, m_tick);
				break;
			case WM_LBUTTONDOWN:
				ProcessKeyMessage(m_mouseMap, VK_LBUTTON, true, m_tick);
				break;
			case WM_LBUTTONUP:
				ProcessKeyMessage(m_mouseMap, VK_LBUTTON, false, m_tick);
				break;
			case WM_RBUTTONDOWN:
				ProcessKeyMessage(m_mouseMap, VK_RBUTTON, true, m_tick);
				break;
			case WM_RBUTTONUP:
				ProcessKeyMessage(m_mouseMap, VK_RBUTTON, false, m_tick);
				break;
			case WM_MBUTTONDOWN:
				ProcessKeyMessage(m_mouseMap, VK_MBUTTON, true, m_tick);
				break;
			case WM_MBUTTONUP:
				ProcessKeyMessage(m_mouseMap, VK_MBUTTON, false, m_tick);
				break;
			case WM_XBUTTONDOWN:
				switch (HIWORD(wParam))
				{
					case XBUTTON1:
						ProcessKeyMessage(m_mouseMap, VK_XBUTTON1, true, m_tick);
						break;
					case XBUTTON2:
						ProcessKeyMessage(m_mouseMap, VK_XBUTTON2, true, m_tick);
						break;
				}
				break;
			case WM_XBUTTONUP:
				switch (HIWORD(wParam))
				{
					case XBUTTON1:
						ProcessKeyMessage(m_mouseMap, VK_XBUTTON1, false, m_tick);
						break;
					case XBUTTON2:
						ProcessKeyMessage(m_mouseMap, VK_XBUTTON2, false, m_tick);
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
		return (*iter).second.first && (*iter).second.second + 1 <= m_tick;
	}

	bool Input::GetKeyDown(int key) const
	{
		auto iter = m_keyMap.find(key);
		if (iter == m_keyMap.end())
		{
			return false;
		}
		return (*iter).second.first && (*iter).second.second + 1 == m_tick;
	}

	bool Input::GetKeyUp(int key) const
	{
		auto iter = m_keyMap.find(key);
		if (iter == m_keyMap.end())
		{
			return false;
		}
		return !(*iter).second.first && (*iter).second.second + 1 == m_tick;
	}

	bool Input::GetMouseButton(int button) const
	{
		auto iter = m_mouseMap.find(button);
		if (iter == m_mouseMap.end())
		{
			return false;
		}
		return (*iter).second.first && (*iter).second.second + 1 <= m_tick;
	}

	bool Input::GetMouseButtonDown(int button) const
	{
		auto iter = m_mouseMap.find(button);
		if (iter == m_mouseMap.end())
		{
			return false;
		}
		return (*iter).second.first && (*iter).second.second + 1 == m_tick;
	}

	bool Input::GetMouseButtonUp(int button) const
	{
		auto iter = m_mouseMap.find(button);
		if (iter == m_mouseMap.end())
		{
			return false;
		}
		return !(*iter).second.first && (*iter).second.second + 1 == m_tick;
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