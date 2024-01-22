#pragma once

#include "pch.h"

template <typename T>
class Singleton
{
private:
	static Singleton* instance;

public:
	static T* GetInstance()
	{
		if (instance == nullptr)
			instance = new T();

		return static_cast<T*>(instance);
	}

	static void DestroyInstance()
	{
		if (instance != nullptr)
		{
			delete instance;
			instance = nullptr;
		}
	}

public:
	Singleton()
	{
		assert(instance == nullptr);
		instance = static_cast<T*>(this);
	}

	virtual ~Singleton()
	{

	}
};