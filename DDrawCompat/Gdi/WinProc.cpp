#include <map>
#include <set>

#include <Windows.h>

#include <Common/Hook.h>
#include <Common/Log.h>
#include <Common/ScopedSrwLock.h>
#include <Dll/Dll.h>
#include <Gdi/AccessGuard.h>
#include <Gdi/Dc.h>
#include <Gdi/PresentationWindow.h>
#include <Gdi/ScrollBar.h>
#include <Gdi/ScrollFunctions.h>
#include <Gdi/TitleBar.h>
#include <Gdi/Window.h>
#include <Gdi/WinProc.h>
#include <Win32/DisplayMode.h>

namespace
{
	struct WindowProc
	{
		WNDPROC wndProcA;
		WNDPROC wndProcW;
	};

	HWINEVENTHOOK g_objectCreateEventHook = nullptr;
	HWINEVENTHOOK g_objectStateChangeEventHook = nullptr;
	std::set<Gdi::WindowPosChangeNotifyFunc> g_windowPosChangeNotifyFuncs;

	Compat::SrwLock g_windowProcSrwLock;
	std::map<HWND, WindowProc> g_windowProc;

	WindowProc getWindowProc(HWND hwnd);
	bool isTopLevelWindow(HWND hwnd);
	void onCreateWindow(HWND hwnd);
	void onDestroyWindow(HWND hwnd);
	void onWindowPosChanged(HWND hwnd);
	void onWindowPosChanging(HWND hwnd, WINDOWPOS& wp);
	void setWindowProc(HWND hwnd, WNDPROC wndProcA, WNDPROC wndProcW);

	LRESULT CALLBACK ddcWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
		decltype(&CallWindowProcA) callWindowProc, WNDPROC wndProc)
	{
		LOG_FUNC("ddcWindowProc", Compat::WindowMessageStruct(hwnd, uMsg, wParam, lParam));

		switch (uMsg)
		{
		case WM_SYNCPAINT:
			if (isTopLevelWindow(hwnd))
			{
				Gdi::Window::onSyncPaint(hwnd);
				return 0;
			}
			break;

		case WM_WINDOWPOSCHANGED:
			onWindowPosChanged(hwnd);
			break;
		}

		LRESULT result = callWindowProc(wndProc, hwnd, uMsg, wParam, lParam);

		switch (uMsg)
		{
		case WM_NCDESTROY:
			onDestroyWindow(hwnd);
			break;

		case WM_STYLECHANGED:
			if (isTopLevelWindow(hwnd))
			{
				Gdi::Window::onStyleChanged(hwnd, wParam);
			}
			break;

		case WM_WINDOWPOSCHANGING:
			onWindowPosChanging(hwnd, *reinterpret_cast<WINDOWPOS*>(lParam));
			break;
		}

		return LOG_RESULT(result);
	}

	LRESULT CALLBACK ddcWindowProcA(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		return ddcWindowProc(hwnd, uMsg, wParam, lParam, CallWindowProcA, getWindowProc(hwnd).wndProcA);
	}

	LRESULT CALLBACK ddcWindowProcW(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		return ddcWindowProc(hwnd, uMsg, wParam, lParam, CallWindowProcW, getWindowProc(hwnd).wndProcW);
	}

	LONG getWindowLong(HWND hWnd, int nIndex,
		decltype(&GetWindowLongA) origGetWindowLong, WNDPROC(WindowProc::* wndProc))
	{
		if (GWL_WNDPROC == nIndex)
		{
			Compat::ScopedSrwLockExclusive lock(g_windowProcSrwLock);
			auto it = g_windowProc.find(hWnd);
			if (it != g_windowProc.end())
			{
				return reinterpret_cast<LONG>(it->second.*wndProc);
			}
		}
		return origGetWindowLong(hWnd, nIndex);
	}

	LONG WINAPI getWindowLongA(HWND hWnd, int nIndex)
	{
		LOG_FUNC("GetWindowLongA", hWnd, nIndex);
		return LOG_RESULT(getWindowLong(hWnd, nIndex, CALL_ORIG_FUNC(GetWindowLongA), &WindowProc::wndProcA));
	}

	LONG WINAPI getWindowLongW(HWND hWnd, int nIndex)
	{
		LOG_FUNC("GetWindowLongW", hWnd, nIndex);
		return LOG_RESULT(getWindowLong(hWnd, nIndex, CALL_ORIG_FUNC(GetWindowLongW), &WindowProc::wndProcW));
	}

	WindowProc getWindowProc(HWND hwnd)
	{
		Compat::ScopedSrwLockExclusive lock(g_windowProcSrwLock);
		return g_windowProc[hwnd];
	}

	BOOL CALLBACK initChildWindow(HWND hwnd, LPARAM /*lParam*/)
	{
		onCreateWindow(hwnd);
		return TRUE;
	}

	BOOL CALLBACK initTopLevelWindow(HWND hwnd, LPARAM /*lParam*/)
	{
		DWORD windowPid = 0;
		GetWindowThreadProcessId(hwnd, &windowPid);
		if (GetCurrentProcessId() == windowPid)
		{
			onCreateWindow(hwnd);
			EnumChildWindows(hwnd, &initChildWindow, 0);
			if (8 == Win32::DisplayMode::getBpp())
			{
				PostMessage(hwnd, WM_PALETTECHANGED, reinterpret_cast<WPARAM>(GetDesktopWindow()), 0);
			}
		}
		return TRUE;
	}

	bool isTopLevelWindow(HWND hwnd)
	{
		return GetDesktopWindow() == GetAncestor(hwnd, GA_PARENT);
	}

	void CALLBACK objectCreateEvent(
		HWINEVENTHOOK /*hWinEventHook*/,
		DWORD /*event*/,
		HWND hwnd,
		LONG idObject,
		LONG /*idChild*/,
		DWORD /*dwEventThread*/,
		DWORD /*dwmsEventTime*/)
	{
		if (OBJID_WINDOW == idObject && !Gdi::PresentationWindow::isPresentationWindow(hwnd))
		{
			onCreateWindow(hwnd);
		}
	}

	void CALLBACK objectStateChangeEvent(
		HWINEVENTHOOK /*hWinEventHook*/,
		DWORD /*event*/,
		HWND hwnd,
		LONG idObject,
		LONG /*idChild*/,
		DWORD /*dwEventThread*/,
		DWORD /*dwmsEventTime*/)
	{
		if (OBJID_TITLEBAR == idObject || OBJID_HSCROLL == idObject || OBJID_VSCROLL == idObject)
		{
			if (!hwnd)
			{
				return;
			}

			HDC windowDc = GetWindowDC(hwnd);
			HDC compatDc = Gdi::Dc::getDc(windowDc);
			if (compatDc)
			{
				Gdi::AccessGuard accessGuard(Gdi::ACCESS_WRITE);
				if (OBJID_TITLEBAR == idObject)
				{
					Gdi::TitleBar(hwnd, compatDc).drawButtons();
				}
				else if (OBJID_HSCROLL == idObject)
				{
					Gdi::ScrollBar(hwnd, compatDc).drawHorizArrows();
				}
				else if (OBJID_VSCROLL == idObject)
				{
					Gdi::ScrollBar(hwnd, compatDc).drawVertArrows();
				}
				Gdi::Dc::releaseDc(windowDc);
			}
			CALL_ORIG_FUNC(ReleaseDC)(hwnd, windowDc);
		}
	}

	void onCreateWindow(HWND hwnd)
	{
		LOG_FUNC("onCreateWindow", hwnd);

		{
			Compat::ScopedSrwLockExclusive lock(g_windowProcSrwLock);
			if (g_windowProc.find(hwnd) != g_windowProc.end())
			{
				return;
			}

			auto wndProcA = reinterpret_cast<WNDPROC>(CALL_ORIG_FUNC(GetWindowLongA)(hwnd, GWL_WNDPROC));
			auto wndProcW = reinterpret_cast<WNDPROC>(CALL_ORIG_FUNC(GetWindowLongW)(hwnd, GWL_WNDPROC));
			g_windowProc[hwnd] = { wndProcA, wndProcW };
			setWindowProc(hwnd, ddcWindowProcA, ddcWindowProcW);
		}

		if (!isTopLevelWindow(hwnd))
		{
			return;
		}

		char className[64] = {};
		GetClassName(hwnd, className, sizeof(className));
		if (std::string(className) == "CompatWindowDesktopReplacement")
		{
			// Disable VirtualizeDesktopPainting shim
			SendNotifyMessage(hwnd, WM_CLOSE, 0, 0);
			return;
		}

		Gdi::Window::updateAll();
	}

	void onDestroyWindow(HWND hwnd)
	{
		if (isTopLevelWindow(hwnd))
		{
			Gdi::Window::updateAll();
			return;
		}

		Compat::ScopedSrwLockExclusive lock(g_windowProcSrwLock);
		auto it = g_windowProc.find(hwnd);
		if (it != g_windowProc.end())
		{
			setWindowProc(hwnd, it->second.wndProcA, it->second.wndProcW);
			g_windowProc.erase(it);
		}
	}

	void onWindowPosChanged(HWND hwnd)
	{
		for (auto notifyFunc : g_windowPosChangeNotifyFuncs)
		{
			notifyFunc();
		}

		if (isTopLevelWindow(hwnd))
		{
			Gdi::Window::updateAll();
		}
	}

	void onWindowPosChanging(HWND hwnd, WINDOWPOS& wp)
	{
		if (isTopLevelWindow(hwnd))
		{
			wp.flags |= SWP_NOREDRAW;
		}
		else
		{
			wp.flags |= SWP_NOCOPYBITS;
		}
	}

	BOOL WINAPI setLayeredWindowAttributes(HWND hwnd, COLORREF crKey, BYTE bAlpha, DWORD dwFlags)
	{
		LOG_FUNC("SetLayeredWindowAttributes", hwnd, crKey, bAlpha, dwFlags);
		BOOL result = CALL_ORIG_FUNC(SetLayeredWindowAttributes)(hwnd, crKey, bAlpha, dwFlags);
		if (result)
		{
			Gdi::Window::updateLayeredWindowInfo(hwnd,
				(dwFlags & LWA_COLORKEY) ? crKey : CLR_INVALID,
				(dwFlags & LWA_ALPHA) ? bAlpha : 255);
		}
		return LOG_RESULT(result);
	}

	LONG setWindowLong(HWND hWnd, int nIndex, LONG dwNewLong,
		decltype(&SetWindowLongA) origSetWindowLong, WNDPROC(WindowProc::* wndProc))
	{
		if (GWL_WNDPROC == nIndex)
		{
			Compat::ScopedSrwLockExclusive lock(g_windowProcSrwLock);
			auto it = g_windowProc.find(hWnd);
			if (it != g_windowProc.end() && 0 != origSetWindowLong(hWnd, nIndex, dwNewLong))
			{
				WNDPROC oldWndProc = it->second.*wndProc;
				it->second.wndProcA = reinterpret_cast<WNDPROC>(CALL_ORIG_FUNC(GetWindowLongA)(hWnd, GWL_WNDPROC));
				it->second.wndProcW = reinterpret_cast<WNDPROC>(CALL_ORIG_FUNC(GetWindowLongW)(hWnd, GWL_WNDPROC));
				WindowProc newWindowProc = { ddcWindowProcA, ddcWindowProcW };
				origSetWindowLong(hWnd, GWL_WNDPROC, reinterpret_cast<LONG>(newWindowProc.*wndProc));
				return reinterpret_cast<LONG>(oldWndProc);
			}
		}
		return origSetWindowLong(hWnd, nIndex, dwNewLong);
	}

	LONG WINAPI setWindowLongA(HWND hWnd, int nIndex, LONG dwNewLong)
	{
		LOG_FUNC("SetWindowLongA", hWnd, nIndex, dwNewLong);
		return LOG_RESULT(setWindowLong(hWnd, nIndex, dwNewLong, CALL_ORIG_FUNC(SetWindowLongA), &WindowProc::wndProcA));
	}

	LONG WINAPI setWindowLongW(HWND hWnd, int nIndex, LONG dwNewLong)
	{
		LOG_FUNC("SetWindowLongW", hWnd, nIndex, dwNewLong);
		return LOG_RESULT(setWindowLong(hWnd, nIndex, dwNewLong, CALL_ORIG_FUNC(SetWindowLongW), &WindowProc::wndProcW));
	}

	BOOL WINAPI setWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags)
	{
		LOG_FUNC("SetWindowPos", hWnd, hWndInsertAfter, X, Y, cx, cy, Compat::hex(uFlags));
		if (uFlags & SWP_NOSENDCHANGING)
		{
			WINDOWPOS wp = {};
			wp.hwnd = hWnd;
			wp.hwndInsertAfter = hWndInsertAfter;
			wp.x = X;
			wp.y = Y;
			wp.cx = cx;
			wp.cy = cy;
			wp.flags = uFlags;
			onWindowPosChanging(hWnd, wp);
			uFlags = wp.flags;
		}
		return LOG_RESULT(CALL_ORIG_FUNC(SetWindowPos)(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags));
	}

	void setWindowProc(HWND hwnd, WNDPROC wndProcA, WNDPROC wndProcW)
	{
		if (IsWindowUnicode(hwnd))
		{
			CALL_ORIG_FUNC(SetWindowLongW)(hwnd, GWL_WNDPROC, reinterpret_cast<LONG>(wndProcW));
		}
		else
		{
			CALL_ORIG_FUNC(SetWindowLongA)(hwnd, GWL_WNDPROC, reinterpret_cast<LONG>(wndProcA));
		}
	}

	BOOL WINAPI updateLayeredWindow(HWND hWnd, HDC hdcDst, POINT* pptDst, SIZE* psize,
		HDC hdcSrc, POINT* pptSrc, COLORREF crKey, BLENDFUNCTION* pblend, DWORD dwFlags)
	{
		LOG_FUNC("UpdateLayeredWindow", hWnd, hdcDst, pptDst, psize, hdcSrc, pptSrc, crKey, pblend, dwFlags);
		BOOL result = CALL_ORIG_FUNC(UpdateLayeredWindow)(
			hWnd, hdcDst, pptDst, psize, hdcSrc, pptSrc, crKey, pblend, dwFlags);
		if (result && hdcSrc)
		{
			Gdi::Window::updateLayeredWindowInfo(hWnd,
				(dwFlags & ULW_COLORKEY) ? crKey : CLR_INVALID,
				((dwFlags & LWA_ALPHA) && pblend) ? pblend->SourceConstantAlpha : 255);
		}
		return LOG_RESULT(result);
	}

	BOOL WINAPI updateLayeredWindowIndirect(HWND hwnd, const UPDATELAYEREDWINDOWINFO* pULWInfo)
	{
		LOG_FUNC("UpdateLayeredWindowIndirect", hwnd, pULWInfo);
		BOOL result = CALL_ORIG_FUNC(UpdateLayeredWindowIndirect)(hwnd, pULWInfo);
		if (result && pULWInfo)
		{
			Gdi::Window::updateLayeredWindowInfo(hwnd,
				(pULWInfo->dwFlags & ULW_COLORKEY) ? pULWInfo->crKey : CLR_INVALID,
				((pULWInfo->dwFlags & LWA_ALPHA) && pULWInfo->pblend) ? pULWInfo->pblend->SourceConstantAlpha : 255);
		}
		return LOG_RESULT(result);
	}
}

namespace Gdi
{
	namespace WinProc
	{
		void dllThreadDetach()
		{
			auto threadId = GetCurrentThreadId();
			Compat::ScopedSrwLockExclusive lock(g_windowProcSrwLock);
			auto it = g_windowProc.begin();
			while (it != g_windowProc.end())
			{
				if (threadId == GetWindowThreadProcessId(it->first, nullptr))
				{
					it = g_windowProc.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		void installHooks()
		{
			HOOK_FUNCTION(user32, GetWindowLongA, getWindowLongA);
			HOOK_FUNCTION(user32, GetWindowLongW, getWindowLongW);
			HOOK_FUNCTION(user32, SetLayeredWindowAttributes, setLayeredWindowAttributes);
			HOOK_FUNCTION(user32, SetWindowLongA, setWindowLongA);
			HOOK_FUNCTION(user32, SetWindowLongW, setWindowLongW);
			HOOK_FUNCTION(user32, SetWindowPos, setWindowPos);
			HOOK_FUNCTION(user32, UpdateLayeredWindow, updateLayeredWindow);
			HOOK_FUNCTION(user32, UpdateLayeredWindowIndirect, updateLayeredWindowIndirect);

			g_objectCreateEventHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE,
				Dll::g_currentModule, &objectCreateEvent, GetCurrentProcessId(), 0, WINEVENT_INCONTEXT);
			g_objectStateChangeEventHook = SetWinEventHook(EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_STATECHANGE,
				Dll::g_currentModule, &objectStateChangeEvent, GetCurrentProcessId(), 0, WINEVENT_INCONTEXT);

			EnumWindows(initTopLevelWindow, 0);
			Gdi::Window::updateAll();
		}

		void onCreateWindow(HWND hwnd)
		{
			if (g_objectCreateEventHook)
			{
				::onCreateWindow(hwnd);
			}
		}

		void watchWindowPosChanges(WindowPosChangeNotifyFunc notifyFunc)
		{
			g_windowPosChangeNotifyFuncs.insert(notifyFunc);
		}

		void uninstallHooks()
		{
			UnhookWinEvent(g_objectStateChangeEventHook);
			UnhookWinEvent(g_objectCreateEventHook);

			Compat::ScopedSrwLockExclusive lock(g_windowProcSrwLock);
			for (const auto& windowProc : g_windowProc)
			{
				setWindowProc(windowProc.first, windowProc.second.wndProcA, windowProc.second.wndProcW);
			}
			g_windowProc.clear();
		}
	}
}
