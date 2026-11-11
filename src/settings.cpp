#include "settings.h"
#include <windows.h>
#include <atomic>

namespace
{
	SRWLOCK s_lock = SRWLOCK_INIT;
	forge::settings s_settings;
	std::atomic<uint64_t> s_generation { 1 };
}

forge::settings forge::get_settings()
{
	AcquireSRWLockShared(&s_lock);
	settings copy = s_settings;
	ReleaseSRWLockShared(&s_lock);
	return copy;
}

void forge::set_settings(const settings &s)
{
	settings n = s;
	if (n.render_scale < 33.4f) n.render_scale = 33.4f;
	if (n.render_scale > 100.0f) n.render_scale = 100.0f;
	if (n.dsr_method > WINE_DSR_NEURAL) n.dsr_method = WINE_DSR_AUTO;
	if (n.dsr_smoothness < -1) n.dsr_smoothness = -1;
	if (n.dsr_smoothness > 100) n.dsr_smoothness = 100;
	if (n.dldsr_smoothness < -1) n.dldsr_smoothness = -1;
	if (n.dldsr_smoothness > 100) n.dldsr_smoothness = 100;

	AcquireSRWLockExclusive(&s_lock);
	const settings &o = s_settings;
	const bool recreate =
		o.enabled != n.enabled ||
		o.preset != n.preset ||
		o.exposure != n.exposure;
	s_settings = n;
	ReleaseSRWLockExclusive(&s_lock);

	if (recreate)
		bump_feature_generation();
}

uint64_t forge::feature_generation()
{
	return s_generation.load(std::memory_order_acquire);
}

void forge::bump_feature_generation()
{
	s_generation.fetch_add(1, std::memory_order_acq_rel);
}

bool forge::wine()
{
	static const bool on_wine = [] {
		HKEY key = nullptr;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Wine", 0, KEY_READ, &key) != ERROR_SUCCESS)
			return false;
		RegCloseKey(key);
		return true;
	}();
	return on_wine;
}
