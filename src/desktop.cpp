#include "desktop.h"
#include "log.h"
#include "ngx_hook.h"
#include "settings.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
	using forge::desktop::mode;
	using forge::desktop::status;

	SRWLOCK g_lock = SRWLOCK_INIT;
	status g_status;
	std::atomic<HWND> g_window { nullptr };
	std::atomic<int> g_resetup { 0 };        // 1 requested, 2 same-size WM_SIZE sent, 3 shorter size sent
	ULONGLONG g_resetup_at = 0;
	uint64_t g_resetup_calls = 0;            // size queries seen when the request went out

	struct guard
	{
		guard() { AcquireSRWLockExclusive(&g_lock); }
		~guard() { ReleaseSRWLockExclusive(&g_lock); }
	};

	void set_reason(const char *fmt, ...)
	{
		char buf[160];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		bool changed = false;
		{
			guard g;
			changed = std::strcmp(g_status.reason, buf) != 0;
			std::memcpy(g_status.reason, buf, sizeof(buf));
		}
		if (changed && buf[0] != '\0')
			FORGE_LOG("desktop: %s", buf);
	}

	bool read_mode(const wchar_t *device, DWORD index, mode &m)
	{
		DEVMODEW dm = {};
		dm.dmSize = sizeof(dm);
		if (!EnumDisplaySettingsExW(device, index, &dm, 0))
			return false;
		m.w = dm.dmPelsWidth;
		m.h = dm.dmPelsHeight;
		m.hz = dm.dmDisplayFrequency;
		return m.w != 0 && m.h != 0;
	}

	// The display's registry mode, the running mode, and every listed size from the registry mode up
	void enumerate(const wchar_t *device, status &st)
	{
		st.count = 0;
		read_mode(device, ENUM_REGISTRY_SETTINGS, st.native);
		read_mode(device, ENUM_CURRENT_SETTINGS, st.current);
		if (st.native.w == 0)
			st.native = st.current;
		mode m;
		for (DWORD i = 0; read_mode(device, i, m); ++i)
		{
			if (m.w < st.native.w || m.h < st.native.h)
				continue;
			unsigned int k = 0;
			while (k < st.count && (st.modes[k].w != m.w || st.modes[k].h != m.h))
				++k;
			if (k < st.count)
			{
				if (m.hz > st.modes[k].hz)
					st.modes[k].hz = m.hz;
				continue;
			}
			if (st.count >= forge::desktop::max_modes)
				continue;
			unsigned int at = st.count++;
			while (at > 0 && static_cast<unsigned long long>(st.modes[at - 1].w) * st.modes[at - 1].h > static_cast<unsigned long long>(m.w) * m.h)
			{
				st.modes[at] = st.modes[at - 1];
				--at;
			}
			st.modes[at] = m;
		}
	}

	const mode *listed(const status &st, unsigned int w, unsigned int h)
	{
		for (unsigned int i = 0; i < st.count; ++i)
			if (st.modes[i].w == w && st.modes[i].h == h)
				return &st.modes[i];
		return nullptr;
	}

	// ChangeDisplaySettingsEx: the registry mode comes back with a null DEVMODE, every other mode goes in as a
	// CDS_FULLSCREEN change that the system drops when the process ends
	bool switch_to(const wchar_t *device, const mode *m, bool registry)
	{
		LONG r;
		if (registry)
		{
			r = ChangeDisplaySettingsExW(device, nullptr, nullptr, 0, nullptr);
		}
		else
		{
			DEVMODEW dm = {};
			dm.dmSize = sizeof(dm);
			dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | (m->hz != 0 ? DM_DISPLAYFREQUENCY : 0);
			dm.dmPelsWidth = m->w;
			dm.dmPelsHeight = m->h;
			dm.dmDisplayFrequency = m->hz;
			r = ChangeDisplaySettingsExW(device, &dm, nullptr, CDS_FULLSCREEN, nullptr);
		}
		if (r != DISP_CHANGE_SUCCESSFUL)
		{
			set_reason("ChangeDisplaySettingsEx to %ux%u returned %ld", m->w, m->h, r);
			return false;
		}
		FORGE_LOG("desktop: %ls switched to %ux%u", device, m->w, m->h);
		return true;
	}
}

bool forge::desktop::available()
{
	return wine();
}

void forge::desktop::attach(HWND window)
{
	if (window == nullptr)
		return;
	const HWND had = g_window.exchange(window);
	if (had != window)
	{
		guard g;
		g_status.attached = true;
	}
}

void forge::desktop::present(unsigned int buffer_w, unsigned int buffer_h)
{
	const HWND h = g_window.load();
	if (h == nullptr)
		return;
	const ULONGLONG now = GetTickCount64();

	// A requested DLSS re-setup: WM_SIZE at the client size; a shorter one when the game ignored that
	if (const int step = g_resetup.load(); step != 0)
	{
		RECT rc = {};
		GetClientRect(h, &rc);
		const unsigned int cw = static_cast<unsigned int>(rc.right - rc.left), ch = static_cast<unsigned int>(rc.bottom - rc.top);
		const uint64_t calls = ngx_hook::get_stats().optimal_calls;
		const WPARAM wp = IsZoomed(h) ? SIZE_MAXIMIZED : SIZE_RESTORED;
		if (step == 1 && cw != 0 && ch != 0)
		{
			g_resetup_at = now;
			g_resetup_calls = calls;
			g_resetup.store(2);
			PostMessageW(h, WM_SIZE, wp, MAKELPARAM(cw, ch));
		}
		else if (step == 2 && calls != g_resetup_calls)
		{
			g_resetup.store(0);
		}
		else if (step == 2 && now - g_resetup_at > 1500 && ch > 16)
		{
			g_resetup_at = now;
			g_resetup.store(3);
			PostMessageW(h, WM_SIZE, wp, MAKELPARAM(cw, ch - 8));
		}
		else if (step == 3 && (buffer_h == ch - 8 || now - g_resetup_at > 1500))
		{
			g_resetup.store(4);
			g_resetup_at = now;
			PostMessageW(h, WM_SIZE, wp, MAKELPARAM(cw, ch));
		}
		else if (step == 4 && (buffer_h == ch || now - g_resetup_at > 1500))
		{
			g_resetup.store(0);
		}
	}

	{
		guard g;
		g_status.buffer_w = buffer_w;
		g_status.buffer_h = buffer_h;
	}
	if (!available())
		return;

	// The desktop is checked for 10 s after the selected mode, the game's buffer size or the first present
	// changes, and left alone otherwise.
	static ULONGLONG last_check = 0, watch_until = 0, last_switch = 0, last_nudge = 0;
	static unsigned int switch_attempts = 0, nudges = 0, seen_w = 0, seen_h = 0, seen_buffer_w = 0, seen_buffer_h = 0;
	static bool first = true;
	static mode last_wanted;
	static status st;
	if (now - last_check < 250)
		return;
	last_check = now;
	const settings s = get_settings();
	if (first || s.desktop_w != seen_w || s.desktop_h != seen_h || buffer_w != seen_buffer_w || buffer_h != seen_buffer_h)
	{
		first = false;
		seen_w = s.desktop_w;
		seen_h = s.desktop_h;
		seen_buffer_w = buffer_w;
		seen_buffer_h = buffer_h;
		watch_until = now + 10000;
	}
	if (now > watch_until)
		return;

	MONITORINFOEXW mi = {};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
	{
		set_reason("GetMonitorInfo failed for the game window");
		return;
	}
	if (wcscmp(st.device, mi.szDevice) != 0)
	{
		wcscpy(st.device, mi.szDevice);
		enumerate(st.device, st);
		char list[512];
		size_t at = 0;
		for (unsigned int i = 0; i < st.count && at + 24 < sizeof(list); ++i)
			at += static_cast<size_t>(snprintf(list + at, sizeof(list) - at, "%s%ux%u@%u", i ? ", " : "", st.modes[i].w, st.modes[i].h, st.modes[i].hz));
		FORGE_LOG("desktop: %ls runs %ux%u@%u; modes from %ux%u up: %s", st.device, st.current.w, st.current.h, st.current.hz, st.native.w, st.native.h, st.count ? list : "(none)");
	}
	else
	{
		read_mode(st.device, ENUM_CURRENT_SETTINGS, st.current);
	}

	// The selected mode: 0 x 0 is the registry mode
	mode wanted = st.native;
	bool registry = true;
	if (s.desktop_w != 0 && s.desktop_h != 0 && (s.desktop_w != st.native.w || s.desktop_h != st.native.h))
	{
		const mode *m = listed(st, s.desktop_w, s.desktop_h);
		if (m == nullptr)
		{
			set_reason("%ux%u is absent from %ls's mode list", s.desktop_w, s.desktop_h, st.device);
		}
		else
		{
			wanted = *m;
			registry = false;
		}
	}
	if (wanted.w != last_wanted.w || wanted.h != last_wanted.h)
	{
		last_wanted = wanted;
		switch_attempts = 0;
		nudges = 0;
	}

	bool in_place = st.current.w == wanted.w && st.current.h == wanted.h;
	if (!in_place && switch_attempts < 3 && now - last_switch > 3000)
	{
		last_switch = now;
		switch_attempts++;
		watch_until = now + 10000;
		if (switch_to(st.device, &wanted, registry))
		{
			read_mode(st.device, ENUM_CURRENT_SETTINGS, st.current);
			in_place = st.current.w == wanted.w && st.current.h == wanted.h;
			if (in_place)
				set_reason("");
			else
				set_reason("the desktop reports %ux%u after the switch to %ux%u", st.current.w, st.current.h, wanted.w, wanted.h);
		}
		GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
	}

	// After a mode change the borderless window and swapchain follow on their own; when they lag, the window
	// gets an asynchronous SetWindowPos and the swapchain a WM_SIZE
	RECT rc = {};
	GetClientRect(h, &rc);
	const unsigned int client_w = static_cast<unsigned int>(rc.right - rc.left), client_h = static_cast<unsigned int>(rc.bottom - rc.top);
	const unsigned int mon_w = static_cast<unsigned int>(mi.rcMonitor.right - mi.rcMonitor.left), mon_h = static_cast<unsigned int>(mi.rcMonitor.bottom - mi.rcMonitor.top);
	const LONG style = GetWindowLongW(h, GWL_STYLE);
	const bool borderless = (style & (WS_POPUP | WS_CAPTION | WS_THICKFRAME)) == WS_POPUP && !IsIconic(h);
	const bool window_fits = client_w == mon_w && client_h == mon_h;
	const bool buffer_fits = buffer_w == client_w && buffer_h == client_h;
	bool request = false;
	if (in_place && borderless && (!window_fits || !buffer_fits) && g_resetup.load() == 0)
	{
		if (now - last_nudge > 1000 && nudges < 3)
		{
			last_nudge = now;
			nudges++;
			request = true;
			watch_until = now + 10000;
			if (!window_fits)
				SetWindowPos(h, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, static_cast<int>(mon_w), static_cast<int>(mon_h), SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
			else
				PostMessageW(h, WM_SIZE, IsZoomed(h) ? SIZE_MAXIMIZED : SIZE_RESTORED, MAKELPARAM(client_w, client_h));
			FORGE_LOG("desktop: game window %ux%u, buffer %ux%u on a %ux%u desktop; %s", client_w, client_h, buffer_w, buffer_h, mon_w, mon_h,
				window_fits ? "WM_SIZE posted" : "window resized to the desktop");
		}
	}

	guard g;
	g_status.attached = true;
	wcscpy(g_status.device, st.device);
	g_status.native = st.native;
	g_status.current = st.current;
	g_status.count = st.count;
	std::memcpy(g_status.modes, st.modes, sizeof(g_status.modes));
	g_status.client_w = client_w;
	g_status.client_h = client_h;
	g_status.borderless = borderless;
	g_status.switched = !registry && in_place;
	if (request)
		g_status.requests++;
}

void forge::desktop::resetup()
{
	if (g_window.load() != nullptr)
		g_resetup.store(1);
}

void forge::desktop::release()
{
	wchar_t device[32] = {};
	bool switched = false;
	{
		guard g;
		switched = g_status.switched;
		wcscpy(device, g_status.device);
		g_status.switched = false;
	}
	if (switched && device[0] != 0)
	{
		const LONG r = ChangeDisplaySettingsExW(device, nullptr, nullptr, 0, nullptr);
		FORGE_LOG("desktop: %ls back to the registry mode (%ld)", device, r);
	}
}

forge::desktop::status forge::desktop::get()
{
	guard g;
	return g_status;
}
