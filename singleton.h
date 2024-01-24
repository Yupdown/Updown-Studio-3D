#pragma once

#include "pch.h"

#define INSTANCE(T) Singleton<T>::GetInstance()

template <typename T> requires std::default_initializable<T>
class Singleton
{
private:
	static T* instance;

public:
	static T* GetInstance()
	{
		if (instance == nullptr)
			CreateInstance();

		return instance;
	}

	template <typename Derived_T = T> requires std::derived_from<Derived_T, T>
	static void CreateInstance()
	{
		instance = new Derived_T();
	}

	static void DestroyInstance()
	{
		delete instance;
		instance = nullptr;
	}

	static bool HasInstance()
	{
		return instance != nullptr;
	}
};

template <typename T> requires std::default_initializable<T>
T* Singleton<T>::instance = nullptr;