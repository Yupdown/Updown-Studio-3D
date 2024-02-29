#include "pch.h"
#include "time_measure.h"

namespace udsdx
{
	TimeMeasure::TimeMeasure()
	{
		m_time = 0.0f;
		m_deltaTime = 0.0f;
	}

	TimeMeasure::~TimeMeasure()
	{

	}

	void TimeMeasure::BeginMeasure()
	{
		m_beginTimerPoint = std::chrono::high_resolution_clock::now();
	}

	void TimeMeasure::EndMeasure()
	{
		auto endTimerPoint = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::duration<float>>(endTimerPoint - m_beginTimerPoint);

		m_deltaTime = duration.count();
		m_time += duration.count();
	}

	Time TimeMeasure::GetTime() const
	{
		return { m_time, m_deltaTime };
	}
}