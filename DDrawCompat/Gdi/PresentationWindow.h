#pragma once

#include <Windows.h>

namespace Gdi
{
	namespace PresentationWindow
	{
		HWND create(HWND owner);
		void destroy(HWND hwnd);
		bool isPresentationWindow(HWND hwnd);
		void setWindowPos(HWND hwnd, const WINDOWPOS& wp);
		void setWindowRgn(HWND hwnd, HRGN rgn);

		void installHooks();
		void uninstallHooks();
	}
}
