#pragma once

#include <chrono>

namespace udsdx
{
	struct Time
	{
		float time;
		float deltaTime;
	};

	class TimeMeasure
	{
	private:
		float m_time;
		float m_deltaTime;

		std::chrono::steady_clock::time_point m_beginTimerPoint;

	public:
		TimeMeasure();
		~TimeMeasure();

		void BeginMeasure();
		void EndMeasure();
		Time GetTime() const;
	};
}