#include <Common/Hook.h>
#include <Common/Log.h>
#include <Dll/Dll.h>
#include <Gdi/PresentationWindow.h>

namespace
{
	const UINT WM_CREATEPRESENTATIONWINDOW = WM_USER;
	const UINT WM_SETPRESENTATIONWINDOWPOS = WM_USER + 1;
	const UINT WM_SETPRESENTATIONWINDOWRGN = WM_USER + 2;

	HANDLE g_presentationWindowThread = nullptr;
	DWORD g_presentationWindowThreadId = 0;
	HWND g_messageWindow = nullptr;

	LRESULT CALLBACK messageWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		LOG_FUNC("messageWindowProc", Compat::WindowMessageStruct(hwnd, uMsg, wParam, lParam));

		switch (uMsg)
		{
		case WM_CREATEPRESENTATIONWINDOW:
		{
			// Workaround for ForceSimpleWindow shim
			static auto origCreateWindowExA = reinterpret_cast<decltype(&CreateWindowExA)>(
				Compat::getProcAddress(GetModuleHandle("user32"), "CreateWindowExA"));

			HWND owner = reinterpret_cast<HWND>(wParam);
			HWND presentationWindow = origCreateWindowExA(
				WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY | WS_EX_TOOLWINDOW,
				"DDrawCompatPresentationWindow",
				nullptr,
				WS_DISABLED | WS_POPUP,
				0, 0, 1, 1,
				owner,
				nullptr,
				nullptr,
				nullptr);

			if (presentationWindow)
			{
				CALL_ORIG_FUNC(SetLayeredWindowAttributes)(presentationWindow, 0, 255, LWA_ALPHA);
			}

			return LOG_RESULT(reinterpret_cast<LRESULT>(presentationWindow));
		}

		case WM_DESTROY:
			PostQuitMessage(0);
			return LOG_RESULT(0);
		}

		return LOG_RESULT(CALL_ORIG_FUNC(DefWindowProc)(hwnd, uMsg, wParam, lParam));
	}

	LRESULT CALLBACK presentationWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		LOG_FUNC("presentationWindowProc", Compat::WindowMessageStruct(hwnd, uMsg, wParam, lParam));

		switch (uMsg)
		{
		case WM_SETPRESENTATIONWINDOWPOS:
		{
			const auto& wp = *reinterpret_cast<WINDOWPOS*>(lParam);
			return SetWindowPos(hwnd, wp.hwndInsertAfter, wp.x, wp.y, wp.cx, wp.cy, wp.flags);
		}

		case WM_SETPRESENTATIONWINDOWRGN:
		{
			HRGN rgn = nullptr;
			if (wParam)
			{
				rgn = CreateRectRgn(0, 0, 0, 0);
				CombineRgn(rgn, reinterpret_cast<HRGN>(wParam), nullptr, RGN_COPY);
			}
			return SetWindowRgn(hwnd, rgn, FALSE);
		}
		}

		return CALL_ORIG_FUNC(DefWindowProc)(hwnd, uMsg, wParam, lParam);
	}

	DWORD WINAPI presentationWindowThreadProc(LPVOID /*lpParameter*/)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

		WNDCLASS wc = {};
		wc.lpfnWndProc = &messageWindowProc;
		wc.hInstance = Dll::g_currentModule;
		wc.lpszClassName = "DDrawCompatMessageWindow";
		CALL_ORIG_FUNC(RegisterClassA)(&wc);

		g_messageWindow = CreateWindow(
			"DDrawCompatMessageWindow", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
		if (!g_messageWindow)
		{
			return 0;
		}

		MSG msg = {};
		while (GetMessage(&msg, nullptr, 0, 0))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		return 0;
	}

	LRESULT sendMessageBlocking(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		DWORD_PTR result = 0;
		SendMessageTimeout(hwnd, msg, wParam, lParam, SMTO_BLOCK | SMTO_NOTIMEOUTIFNOTHUNG, 0, &result);
		return result;
	}
}

namespace Gdi
{
	namespace PresentationWindow
	{
		HWND create(HWND owner)
		{
			return reinterpret_cast<HWND>(sendMessageBlocking(
				g_messageWindow, WM_CREATEPRESENTATIONWINDOW, reinterpret_cast<WPARAM>(owner), 0));
		}

		void destroy(HWND hwnd)
		{
			sendMessageBlocking(hwnd, WM_CLOSE, 0, 0);
		}

		void installHooks()
		{
			WNDCLASS wc = {};
			wc.lpfnWndProc = &presentationWindowProc;
			wc.hInstance = Dll::g_currentModule;
			wc.lpszClassName = "DDrawCompatPresentationWindow";
			CALL_ORIG_FUNC(RegisterClassA)(&wc);

			g_presentationWindowThread = CreateThread(
				nullptr, 0, &presentationWindowThreadProc, nullptr, 0, &g_presentationWindowThreadId);

			int i = 0;
			while (!g_messageWindow && i < 1000)
			{
				Sleep(1);
				++i;
			}

			if (!g_messageWindow)
			{
				Compat::Log() << "ERROR: Failed to create a message-only window";
			}
		}

		bool isPresentationWindow(HWND hwnd)
		{
			return GetWindowThreadProcessId(hwnd, nullptr) == g_presentationWindowThreadId;
		}

		void setWindowPos(HWND hwnd, const WINDOWPOS& wp)
		{
			sendMessageBlocking(hwnd, WM_SETPRESENTATIONWINDOWPOS, 0, reinterpret_cast<WPARAM>(&wp));
		}

		void setWindowRgn(HWND hwnd, HRGN rgn)
		{
			sendMessageBlocking(hwnd, WM_SETPRESENTATIONWINDOWRGN, reinterpret_cast<WPARAM>(rgn), 0);
		}

		void uninstallHooks()
		{
			if (g_presentationWindowThread)
			{
				sendMessageBlocking(g_messageWindow, WM_CLOSE, 0, 0);
				if (WAIT_OBJECT_0 != WaitForSingleObject(g_presentationWindowThread, 1000))
				{
					TerminateThread(g_presentationWindowThread, 0);
					Compat::Log() << "The presentation window thread was terminated forcefully";
				}
			}
		}
	}
}
