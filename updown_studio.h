#pragma once

#include "singleton.h"

namespace udstd
{
	class UpdownStudio : public Singleton<UpdownStudio>
	{
	public:
		UpdownStudio();
		~UpdownStudio();

	public:
		static void Initialize();
		static void Update();
	};
}