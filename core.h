#pragma once

#include "pch.h"

namespace udsdx
{
	class Core
	{
	private:
		std::unique_ptr<Microsoft::WRL::Wrappers::RoInitializeWrapper> m_roInitialization;

	public:
		Core();
		~Core();

		void Initialize();
		void Update();
		bool ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	};
}

