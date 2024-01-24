#pragma once

#include "pch.h"

namespace udsdx
{
	class Input
	{
	private:
		template <typename TKey>
		using KeyMap = std::map<TKey, std::pair<bool, unsigned long long>>;

	public:
		Input();
		~Input();

		void Initialize();
		void IncreaseTick();
		bool ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

		bool GetKey(int key) const;
		bool GetKeyDown(int key) const;
		bool GetKeyUp(int key) const;

		bool GetMouseButton(int button) const;
		bool GetMouseButtonDown(int button) const;
		bool GetMouseButtonUp(int button) const;

		int GetMouseX() const;
		int GetMouseY() const;

	private:
		unsigned long long m_tick;

		KeyMap<int> m_keyMap;
		KeyMap<int> m_mouseMap;

		int m_mouseX;
		int m_mouseY;
	};
}

