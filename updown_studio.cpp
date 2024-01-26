#include "pch.h"
#include "updown_studio.h"

#include "singleton.h"
#include "resource.h"
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
	INSTANCE(Resource)->Initialize();
	INSTANCE(Input)->Initialize();
}

void UpdownStudio::Update()
{
	// TODO: Add your update logic here

	INSTANCE(Input)->IncreaseTick();
}

bool UpdownStudio::ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	int w, h;

	switch (message)
	{
	case WM_SIZE:
		w = LOWORD(lParam);
		h = HIWORD(lParam);
		break;

	default:
		return INSTANCE(Input)->ProcessMessage(hWnd, message, wParam, lParam);
	}

	return true;
}
