#pragma once

namespace forge::log
{
	// Opens (truncates) the log file. Safe to call once; later calls are ignored.
	void open(const wchar_t *path);
	void write(const char *fmt, ...);
	void close();
}

#define FORGE_LOG(...) ::forge::log::write(__VA_ARGS__)
