#include "log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace
{
	SRWLOCK s_lock = SRWLOCK_INIT;
	HANDLE s_file = INVALID_HANDLE_VALUE;
}

void forge::log::open(const wchar_t *path)
{
	AcquireSRWLockExclusive(&s_lock);
	if (s_file == INVALID_HANDLE_VALUE)
		s_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	ReleaseSRWLockExclusive(&s_lock);
}

void forge::log::write(const char *fmt, ...)
{
	char line[2048];
	SYSTEMTIME t;
	GetLocalTime(&t);
	int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

	va_list args;
	va_start(args, fmt);
	int m = vsnprintf(line + n, sizeof(line) - n - 2, fmt, args);
	va_end(args);
	if (m < 0)
		m = 0;
	n += (m < static_cast<int>(sizeof(line)) - n - 2) ? m : static_cast<int>(sizeof(line)) - n - 3;
	line[n++] = '\r';
	line[n++] = '\n';

	AcquireSRWLockExclusive(&s_lock);
	if (s_file != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		WriteFile(s_file, line, static_cast<DWORD>(n), &written, nullptr);
	}
	ReleaseSRWLockExclusive(&s_lock);
}

void forge::log::close()
{
	AcquireSRWLockExclusive(&s_lock);
	if (s_file != INVALID_HANDLE_VALUE)
		CloseHandle(s_file);
	s_file = INVALID_HANDLE_VALUE;
	ReleaseSRWLockExclusive(&s_lock);
}
