#pragma once

#include "pch.h"

namespace udsdx
{
	class Input
	{
	private:
		template <typename T>
		struct KeyMap
		{
		public:
			void SetKey(T key, bool state, unsigned long long tick)
			{
				if (state)
				{
					keyDownTick[key] = tick;
				}
				else
				{
					keyUpTick[key] = tick;
				}
			}

			bool GetKey(T key, unsigned long long tick) const
			{
				auto iterDown = keyDownTick.find(key);
				if (iterDown == keyDownTick.end())
				{
					return false;
				}
				auto iterUp = keyUpTick.find(key);
				if (iterUp == keyUpTick.end())
				{
					return true;
				}
				if (iterDown->second > iterUp->second)
				{
					return true;
				}
				return iterDown->second == tick;
			}

			bool GetKeyDown(T key, unsigned long long tick) const
			{
				auto iter = keyDownTick.find(key);
				if (iter == keyDownTick.end())
				{
					return false;
				}
				return iter->second == tick;
			}

			bool GetKeyUp(T key, unsigned long long tick) const
			{
				auto iter = keyUpTick.find(key);
				if (iter == keyUpTick.end())
				{
					return false;
				}
				return iter->second == tick;
			}

			void Clear()
			{
				keyDownTick.clear();
				keyUpTick.clear();
			}

		private:
			std::unordered_map<T, unsigned long long> keyDownTick;
			std::unordered_map<T, unsigned long long> keyUpTick;
		};

	public:
		Input();
		~Input();

		bool ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
		void FlushQueue();
		void Reset();

		// Check if a key is pressed
		bool GetKey(int key) const;
		// Check if a key is pressed this frame
		bool GetKeyDown(int key) const;
		// Check if a key is released this frame
		bool GetKeyUp(int key) const;

		// Check if a mouse button is pressed
		bool GetMouseButton(int button) const;
		// Check if a mouse button is pressed this frame
		bool GetMouseButtonDown(int button) const;
		// Check if a mouse button is released this frame
		bool GetMouseButtonUp(int button) const;

		// Mouse x position in screen space (up-left origin)
		int GetMouseX() const;
		// Mouse y position in screen space (up-left origin)
		int GetMouseY() const;

	private:
		unsigned long long m_tick = 0ull;

		std::queue<std::tuple<HWND, UINT, WPARAM, LPARAM>> m_messageQueue;
		std::mutex m_queueLock;

		KeyMap<int> m_keyMap;
		KeyMap<int> m_mouseMap;

		int m_mouseX = 0;
		int m_mouseY = 0;
	};
}

