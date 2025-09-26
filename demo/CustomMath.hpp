#pragma once

#include <math.h>

constexpr float F_PI = 3.14159265358979323846f;
constexpr float F_DEG2RAD = F_PI / 180.0f;
constexpr float F_RAD2DEG = 180.0f / F_PI;

inline float Floor(float value)
{
	return floorf(value);
}

inline int FloorToInt(float value)
{
	return static_cast<int>(floorf(value));
}

inline float Ceil(float value)
{
	return ceilf(value);
}

inline int CeilToInt(float value)
{
	return static_cast<int>(ceilf(value));
}

inline float Round(float value)
{
	return roundf(value);
}

inline int RoundToInt(float value)
{
	return static_cast<int>(roundf(value));
}

inline float Sign(float value)
{
	return value >= 0.0f ? 1.0f : -1.0f;
}

inline float Max(float lhs, float rhs)
{
	return lhs > rhs ? lhs : rhs;
}

inline float Min(float lhs, float rhs)
{
	return lhs < rhs ? lhs : rhs;
}

inline float Clamp(float value, float min, float max)
{
	return value > max ? max : value < min ? min : value;
}

inline float Clamp01(float value)
{
	return Clamp(value, 0.0f, 1.0f);
}

inline float Repeat(float t, float length)
{
	return t - floorf(t / length) * length;
}

inline float Lerp(float from, float to, float t)
{
	return from + (to - from) * t;
}

inline float InverseLerp(float from, float to, float value)
{
	return (value - from) / (to - from);
}