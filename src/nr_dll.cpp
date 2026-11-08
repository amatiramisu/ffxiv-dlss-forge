#include "nr_dll.h"
#include "log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace forge::nr
{
	namespace
	{
		struct section
		{
			char name[9];
			uint32_t va, vsize, raw, rsize;
		};

		struct image
		{
			HANDLE file = INVALID_HANDLE_VALUE;
			uint64_t base = 0;
			std::vector<section> sections;
			uint32_t rsrc_rva = 0, rsrc_size = 0;

			bool read(uint64_t offset, void *out, uint32_t size) const
			{
				LARGE_INTEGER at;
				at.QuadPart = static_cast<LONGLONG>(offset);
				if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN))
					return false;
				DWORD got = 0;
				return ReadFile(file, out, size, &got, nullptr) && got == size;
			}

			const section *containing(uint32_t rva) const
			{
				for (const section &s : sections)
					if (rva >= s.va && rva < s.va + (s.vsize > s.rsize ? s.vsize : s.rsize))
						return &s;
				return nullptr;
			}
		};

		// DOS header, PE32+ optional header, section table
		bool open_image(image &im, const wchar_t *path)
		{
			im.file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
			if (im.file == INVALID_HANDLE_VALUE)
				return false;
			uint8_t head[4096];
			if (!im.read(0, head, sizeof(head)) || std::memcmp(head, "MZ", 2) != 0)
				return false;
			uint32_t pe;
			std::memcpy(&pe, head + 0x3c, 4);
			if (pe + 24 + 240 > sizeof(head) || std::memcmp(head + pe, "PE\0\0", 4) != 0)
				return false;
			uint16_t nsec, optsize, magic;
			std::memcpy(&nsec, head + pe + 6, 2);
			std::memcpy(&optsize, head + pe + 20, 2);
			std::memcpy(&magic, head + pe + 24, 2);
			if (magic != 0x20b || optsize < 240 || nsec == 0 || nsec > 32)
				return false;
			std::memcpy(&im.base, head + pe + 24 + 24, 8);
			std::memcpy(&im.rsrc_rva, head + pe + 24 + 112 + 2 * 8, 4);
			std::memcpy(&im.rsrc_size, head + pe + 24 + 112 + 2 * 8 + 4, 4);
			uint32_t off = pe + 24 + optsize;
			if (off + nsec * 40u > sizeof(head))
				return false;
			for (uint16_t i = 0; i < nsec; ++i, off += 40)
			{
				section s = {};
				std::memcpy(s.name, head + off, 8);
				std::memcpy(&s.vsize, head + off + 8, 4);
				std::memcpy(&s.va, head + off + 12, 4);
				std::memcpy(&s.rsize, head + off + 16, 4);
				std::memcpy(&s.raw, head + off + 20, 4);
				im.sections.push_back(s);
			}
			return true;
		}

		// RCDATA entries by name with their sizes; only the .rsrc directory is read
		struct rcdata
		{
			char name[32];
			uint64_t bytes;
		};

		void read_rcdata(const image &im, std::vector<rcdata> &out)
		{
			const section *s = im.containing(im.rsrc_rva);
			if (s == nullptr)
				return;
			const uint32_t window = s->rsize < (1u << 20) ? s->rsize : (1u << 20);
			std::vector<uint8_t> dir(window);
			if (!im.read(s->raw + (im.rsrc_rva - s->va), dir.data(), window))
				return;
			const auto u16 = [&](uint32_t at) -> uint32_t { uint16_t v = 0; if (at + 2 <= window) std::memcpy(&v, dir.data() + at, 2); return v; };
			const auto u32 = [&](uint32_t at) -> uint32_t { uint32_t v = 0; if (at + 4 <= window) std::memcpy(&v, dir.data() + at, 4); return v; };
			const auto entries = [&](uint32_t table) { return u16(table + 12) + u16(table + 14); };
			// level 1: types
			for (uint32_t i = 0, n = entries(0); i < n && i < 64; ++i)
			{
				const uint32_t id = u32(16 + i * 8), sub = u32(16 + i * 8 + 4);
				if (id != 10 || (sub & 0x80000000u) == 0) // RT_RCDATA, a subdirectory
					continue;
				const uint32_t names = sub & 0x7fffffffu;
				for (uint32_t j = 0, m = entries(names); j < m && j < 64; ++j)
				{
					const uint32_t name = u32(names + 16 + j * 8), langs = u32(names + 16 + j * 8 + 4);
					if ((name & 0x80000000u) == 0 || (langs & 0x80000000u) == 0)
						continue;
					rcdata r = {};
					const uint32_t str = name & 0x7fffffffu;
					const uint32_t len = u16(str);
					for (uint32_t k = 0; k < len && k + 1 < sizeof(r.name); ++k)
					{
						const uint32_t c = u16(str + 2 + k * 2);
						r.name[k] = c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
					}
					const uint32_t lang = langs & 0x7fffffffu;
					if (entries(lang) == 0)
						continue;
					const uint32_t data = u32(lang + 16 + 4);
					if (data & 0x80000000u)
						continue;
					r.bytes = u32(data + 4);
					out.push_back(r);
				}
			}
		}

		// The descriptor table in .rdata: per entry pointers to the name, the preset as an int, pointers to the
		// resource name and a tag. Entries are found by the resource name pointer and checked by the strings.
		struct rdata
		{
			const image *im;
			const section *s;
			std::vector<uint8_t> bytes;

			const char *string_at(uint64_t va) const
			{
				if (va < im->base + s->va || va >= im->base + s->va + bytes.size())
					return nullptr;
				const size_t off = static_cast<size_t>(va - im->base - s->va);
				for (size_t i = off; i < bytes.size() && i < off + 200; ++i)
				{
					if (bytes[i] == 0)
						return i > off ? reinterpret_cast<const char *>(bytes.data() + off) : nullptr;
					if (bytes[i] < 0x20 || bytes[i] > 0x7e)
						return nullptr;
				}
				return nullptr;
			}
		};

		bool find_entry(const rdata &rd, const char *resource, network &n)
		{
			const size_t len = std::strlen(resource);
			for (size_t pos = 0; pos + len + 1 <= rd.bytes.size(); ++pos)
			{
				if (std::memcmp(rd.bytes.data() + pos, resource, len + 1) != 0 || (pos > 0 && rd.bytes[pos - 1] != 0))
					continue;
				const uint64_t va = rd.im->base + rd.s->va + pos;
				uint8_t ptr[8];
				std::memcpy(ptr, &va, 8);
				for (size_t at = 0x10; at + 0x10 <= rd.bytes.size(); at += 8)
				{
					if (std::memcmp(rd.bytes.data() + at, ptr, 8) != 0)
						continue;
					const size_t entry = at - 0x10;
					uint64_t name_va, tag_va;
					int32_t preset;
					std::memcpy(&name_va, rd.bytes.data() + entry, 8);
					std::memcpy(&preset, rd.bytes.data() + entry + 8, 4);
					std::memcpy(&tag_va, rd.bytes.data() + entry + 0x18, 8);
					const char *name = rd.string_at(name_va), *tag = rd.string_at(tag_va);
					if (name == nullptr || tag == nullptr || preset < 0 || preset > 63)
						continue;
					n.preset = preset;
					snprintf(n.name, sizeof(n.name), "%s", name);
					snprintf(n.resource, sizeof(n.resource), "%s", resource);
					return true;
				}
			}
			return false;
		}

		void read_version(const wchar_t *path, char *out, size_t size)
		{
			out[0] = 0;
			DWORD handle = 0;
			const DWORD n = GetFileVersionInfoSizeW(path, &handle);
			if (n == 0)
				return;
			std::vector<uint8_t> buf(n);
			if (!GetFileVersionInfoW(path, 0, n, buf.data()))
				return;
			VS_FIXEDFILEINFO *info = nullptr;
			UINT len = 0;
			if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void **>(&info), &len) || info == nullptr || len < sizeof(*info))
				return;
			snprintf(out, size, "%u.%u.%u", static_cast<unsigned>(HIWORD(info->dwFileVersionMS)), static_cast<unsigned>(LOWORD(info->dwFileVersionMS)),
				static_cast<unsigned>(HIWORD(info->dwFileVersionLS)));
		}

		dll_info g_info;
		bool g_parsed = false;
		wchar_t g_parsed_path[260] = {};
	}

	void parse_snippet(const wchar_t *path, dll_info &d)
	{
		d = dll_info {};
		wcsncpy(d.path, path, 259);
		{
			image im;
			if (!open_image(im, d.path))
			{
				if (im.file != INVALID_HANDLE_VALUE)
					CloseHandle(im.file);
				return;
			}
			d.found = true;
			read_version(d.path, d.version, sizeof(d.version));
			std::vector<rcdata> resources;
			read_rcdata(im, resources);
			rdata rd = { &im, nullptr, {} };
			for (const section &s : im.sections)
				if (std::strcmp(s.name, ".rdata") == 0)
					rd.s = &s;
			if (rd.s != nullptr)
			{
				rd.bytes.resize(rd.s->rsize);
				if (!im.read(rd.s->raw, rd.bytes.data(), rd.s->rsize))
					rd.bytes.clear();
			}
			for (const rcdata &r : resources)
			{
				if (std::strncmp(r.name, "WEIGHTS_", 8) != 0 || d.count >= 8)
					continue;
				network n;
				if (!rd.bytes.empty() && find_entry(rd, r.name, n))
				{
					d.table = true;
				}
				else
				{
					n.preset = -1;
					snprintf(n.resource, sizeof(n.resource), "%s", r.name);
				}
				n.bytes = r.bytes;
				d.networks[d.count++] = n;
			}
			// by preset; entries without one (table not found) keep the resource order
			for (unsigned int i = 1; i < d.count; ++i)
				for (unsigned int j = i; j > 0 && d.networks[j - 1].preset > d.networks[j].preset; --j)
				{
					const network t = d.networks[j];
					d.networks[j] = d.networks[j - 1];
					d.networks[j - 1] = t;
				}
			CloseHandle(im.file);
		}
	}

	const dll_info &snippet_info()
	{
		wchar_t path[260] = {};
		HMODULE loaded = GetModuleHandleW(L"nvngx_dlssnr.dll");
		if (loaded == nullptr || GetModuleFileNameW(loaded, path, 260) == 0)
		{
			GetModuleFileNameW(nullptr, path, 260);
			if (wchar_t *slash = wcsrchr(path, L'\\'))
				wcscpy(slash + 1, L"nvngx_dlssnr.dll");
		}
		if (g_parsed && wcscmp(path, g_parsed_path) == 0)
			return g_info;
		g_parsed = true;
		wcscpy(g_parsed_path, path);
		parse_snippet(path, g_info);
		if (!g_info.found)
		{
			FORGE_LOG("DLSS NR: no readable nvngx_dlssnr.dll at %ls", path);
			return g_info;
		}
		FORGE_LOG("DLSS NR snippet %s at %ls: %u weight set%s%s", g_info.version[0] ? g_info.version : "(no version)", path, g_info.count,
			g_info.count == 1 ? "" : "s", g_info.table ? "" : " (descriptor table not located; presets unknown)");
		for (unsigned int i = 0; i < g_info.count; ++i)
		{
			const network &n = g_info.networks[i];
			FORGE_LOG("  preset %d: %s (%s, %.1f MB)", n.preset, n.name[0] ? n.name : "?", n.resource, static_cast<double>(n.bytes) / 1e6);
		}
		return g_info;
	}
}
