// Reads the weight table of one or more nvngx_dlssnr.dll files on the CPU alone and writes it to
// forge-dllinfo.log: paths from the command line, otherwise nvngx_dlssnr.dll next to the program.
#include "log.h"
#include "nr_dll.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
	wchar_t log_path[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, log_path, MAX_PATH);
	wcscpy(wcsrchr(log_path, L'.'), L".log");
	forge::log::open(log_path);

	int failures = 0;
	const auto report = [&](const wchar_t *path) {
		forge::nr::dll_info d;
		forge::nr::parse_snippet(path, d);
		FORGE_LOG("%ls: %s, version %s, table %s, %u weight set%s", path, d.found ? "read" : "unreadable", d.version[0] ? d.version : "(none)",
			d.table ? "located" : "absent", d.count, d.count == 1 ? "" : "s");
		for (unsigned int i = 0; i < d.count; ++i)
			FORGE_LOG("  preset %d: %s (%s, %llu bytes)", d.networks[i].preset, d.networks[i].name, d.networks[i].resource,
				static_cast<unsigned long long>(d.networks[i].bytes));
		if (!d.found || !d.table || d.count == 0)
			failures++;
	};
	if (argc > 1)
	{
		for (int i = 1; i < argc; ++i)
		{
			wchar_t path[MAX_PATH] = {};
			MultiByteToWideChar(CP_ACP, 0, argv[i], -1, path, MAX_PATH);
			report(path);
		}
	}
	else
	{
		wchar_t path[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		wcscpy(wcsrchr(path, L'\\') + 1, L"nvngx_dlssnr.dll");
		report(path);
	}
	FORGE_LOG("%s: %d failure(s)", failures ? "FAILED" : "ALL PASSED", failures);
	forge::log::close();
	return failures ? 1 : 0;
}
