#include <map>
#include <vector>

#include <Common/CompatRef.h>
#include <D3dDdi/KernelModeThunks.h>
#include <DDraw/DirectDrawClipper.h>
#include <Gdi/Gdi.h>
#include <Gdi/Region.h>

namespace
{
	struct ClipperData
	{
		HWND hwnd;
		std::vector<unsigned char> oldClipList;
	};

	std::map<IDirectDrawClipper*, ClipperData> g_clipperData;
	bool g_isInvalidated = false;

	void updateWindowClipList(CompatRef<IDirectDrawClipper> clipper, ClipperData& data);

	void onWindowPosChange()
	{
		g_isInvalidated = true;
	}

	void updateWindowClipList(CompatRef<IDirectDrawClipper> clipper, ClipperData& data)
	{
		HDC dc = GetDCEx(data.hwnd, nullptr, DCX_CACHE | DCX_USESTYLE);
		Gdi::Region rgn;
		GetRandomRgn(dc, rgn, SYSRGN);
		CALL_ORIG_FUNC(ReleaseDC)(data.hwnd, dc);

		RECT primaryRect = D3dDdi::KernelModeThunks::getMonitorRect();
		if (0 != primaryRect.left || 0 != primaryRect.top)
		{
			rgn.offset(-primaryRect.left, -primaryRect.top);
		}

		DWORD rgnSize = GetRegionData(rgn, 0, nullptr);
		std::vector<unsigned char> rgnData(rgnSize);
		GetRegionData(rgn, rgnSize, reinterpret_cast<RGNDATA*>(rgnData.data()));

		clipper->SetHWnd(&clipper, 0, nullptr);
		if (FAILED(clipper->SetClipList(&clipper, reinterpret_cast<RGNDATA*>(rgnData.data()), 0)))
		{
			clipper->SetHWnd(&clipper, 0, data.hwnd);
		}
	}

	HRESULT STDMETHODCALLTYPE GetHWnd(IDirectDrawClipper* This, HWND* lphWnd)
	{
		if (lphWnd)
		{
			auto it = g_clipperData.find(This);
			if (it != g_clipperData.end())
			{
				*lphWnd = it->second.hwnd;
				return DD_OK;
			}
		}
		return DDraw::DirectDrawClipper::s_origVtable.GetHWnd(This, lphWnd);
	}
	
	ULONG STDMETHODCALLTYPE Release(IDirectDrawClipper* This)
	{
		ULONG result = DDraw::DirectDrawClipper::s_origVtable.Release(This);
		if (0 == result)
		{
			g_clipperData.erase(This);
		}
		return result;
	}

	HRESULT STDMETHODCALLTYPE SetClipList(IDirectDrawClipper* This, LPRGNDATA lpClipList, DWORD dwFlags)
	{
		if (g_clipperData.find(This) != g_clipperData.end())
		{
			return DDERR_CLIPPERISUSINGHWND;
		}
		return DDraw::DirectDrawClipper::s_origVtable.SetClipList(This, lpClipList, dwFlags);
	}

	HRESULT STDMETHODCALLTYPE SetHWnd(IDirectDrawClipper* This, DWORD dwFlags, HWND hWnd)
	{
		const auto& origVtable = DDraw::DirectDrawClipper::s_origVtable;

		HRESULT result = origVtable.SetHWnd(This, dwFlags, hWnd);
		if (SUCCEEDED(result))
		{
			auto it = g_clipperData.find(This);
			if (hWnd)
			{
				if (it == g_clipperData.end())
				{
					it = g_clipperData.insert({ This, ClipperData() }).first;
					it->second.hwnd = hWnd;

					DWORD size = 0;
					origVtable.GetClipList(This, nullptr, nullptr, &size);
					it->second.oldClipList.resize(size);
					origVtable.GetClipList(This, nullptr,
						reinterpret_cast<RGNDATA*>(it->second.oldClipList.data()), &size);
				}
				updateWindowClipList(*This, it->second);
				Gdi::watchWindowPosChanges(&onWindowPosChange);
			}
			else if (it != g_clipperData.end())
			{
				origVtable.SetClipList(This, it->second.oldClipList.empty() ? nullptr :
					reinterpret_cast<RGNDATA*>(it->second.oldClipList.data()), 0);
				g_clipperData.erase(it);
			}
		}
		return result;
	}
}

namespace DDraw
{
	HRGN DirectDrawClipper::getClipRgn(CompatRef<IDirectDrawClipper> clipper)
	{
		std::vector<unsigned char> rgnData;
		DWORD size = 0;
		clipper->GetClipList(&clipper, nullptr, nullptr, &size);
		rgnData.resize(size);
		clipper->GetClipList(&clipper, nullptr, reinterpret_cast<RGNDATA*>(rgnData.data()), &size);
		return ExtCreateRegion(nullptr, size, reinterpret_cast<RGNDATA*>(rgnData.data()));
	}

	HRESULT DirectDrawClipper::setClipRgn(CompatRef<IDirectDrawClipper> clipper, HRGN rgn)
	{
		std::vector<unsigned char> rgnData;
		rgnData.resize(GetRegionData(rgn, 0, nullptr));
		GetRegionData(rgn, rgnData.size(), reinterpret_cast<RGNDATA*>(rgnData.data()));
		return clipper->SetClipList(&clipper, reinterpret_cast<RGNDATA*>(rgnData.data()), 0);
	}

	void DirectDrawClipper::setCompatVtable(IDirectDrawClipperVtbl& vtable)
	{
		vtable.GetHWnd = &GetHWnd;
		vtable.Release = &Release;
		vtable.SetClipList = &SetClipList;
		vtable.SetHWnd = &SetHWnd;
	}

	void DirectDrawClipper::update()
	{
		if (g_isInvalidated)
		{
			g_isInvalidated = false;
			for (auto& clipperData : g_clipperData)
			{
				updateWindowClipList(*clipperData.first, clipperData.second);
			}
		}
	}
}
