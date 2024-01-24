#include "pch.h"
#include "updown_studio.h"

#include "singleton.h"
#include "input.h"

using namespace udsdx;

UpdownStudio::UpdownStudio()
{

}

UpdownStudio::~UpdownStudio()
{

}

void UpdownStudio::Initialize()
{
	INSTANCE(Input)->Initialize();
}

void UpdownStudio::Update()
{
	// TODO: Add your update logic here

	INSTANCE(Input)->IncreaseTick();
}

bool UpdownStudio::ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	return INSTANCE(Input)->ProcessMessage(hWnd, message, wParam, lParam);
}
