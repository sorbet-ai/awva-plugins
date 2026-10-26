// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
#include "platform.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#else
#include <dlfcn.h>
#include <time.h>
#endif

namespace fs = std::filesystem;

namespace awva_app {

uint64_t now_ns() {
#if defined(_WIN32)
	static const double scale = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return 1e9 / static_cast<double>(f.QuadPart);
	}();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return static_cast<uint64_t>(static_cast<double>(c.QuadPart) * scale);
#else
	timespec ts;
	clock_gettime(CLOCK_UPTIME_RAW, &ts); // (mach_absolute_time's clock, CoreMIDI's and CoreAudio's host time)
	return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

#if defined(_WIN32)
static std::string utf8_of(const std::wstring& w) {
	if (w.empty()) return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
	return s;
}
static std::string known_folder(REFKNOWNFOLDERID id) {
	PWSTR p = nullptr;
	std::string s;
	if (SHGetKnownFolderPath(id, 0, nullptr, &p) == S_OK) s = utf8_of(p);
	CoTaskMemFree(p);
	return s;
}
#endif

static fs::path P(const std::string& s) { return fs::u8path(s); }

std::string core_dir() {
#if defined(_WIN32)
	HMODULE m = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	    reinterpret_cast<LPCWSTR>(&core_dir), &m);
	wchar_t buf[4096];
	const DWORD n = GetModuleFileNameW(m, buf, 4096);
	return P(utf8_of(std::wstring(buf, n))).parent_path().u8string();
#else
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&core_dir), &info) && info.dli_fname) return P(info.dli_fname).parent_path().u8string();
	return ".";
#endif
}

std::string app_data_dir() {
#if defined(_WIN32)
	return join(known_folder(FOLDERID_RoamingAppData), "awva");
#else
	const char* h = std::getenv("HOME");
	return join(join(h ? h : ".", "Library/Application Support"), "awva");
#endif
}

std::string cache_dir() {
#if defined(_WIN32)
	return join(join(known_folder(FOLDERID_LocalAppData), "awva"), "cache");
#else
	const char* h = std::getenv("HOME");
	return join(join(h ? h : ".", "Library/Caches"), "awva");
#endif
}

bool read_file(const std::string& path, std::string* out, size_t limit) {
	out->clear();
#if defined(_WIN32)
	FILE* f = _wfopen(P(path).c_str(), L"rb");
#else
	FILE* f = std::fopen(path.c_str(), "rb");
#endif
	if (!f) return false;
	char buf[8192];
	size_t got;
	while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) {
		out->append(buf, got);
		if (out->size() > limit) {
			std::fclose(f);
			return false;
		}
	}
	std::fclose(f);
	return true;
}

bool write_file(const std::string& path, const std::string& text) {
	std::error_code ec;
	fs::create_directories(P(path).parent_path(), ec);
	const fs::path tmp = P(path + ".tmp");
#if defined(_WIN32)
	FILE* f = _wfopen(tmp.c_str(), L"wb");
#else
	FILE* f = std::fopen(tmp.c_str(), "wb");
#endif
	if (!f) return false;
	const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	std::fclose(f);
	if (!ok) return false;
	fs::rename(tmp, P(path), ec);
	return !ec;
}

bool copy_file(const std::string& from, const std::string& to) {
	std::error_code ec;
	fs::create_directories(P(to).parent_path(), ec);
	fs::copy_file(P(from), P(to), fs::copy_options::overwrite_existing, ec);
	return !ec;
}

bool make_dirs(const std::string& path) {
	std::error_code ec;
	fs::create_directories(P(path), ec);
	return fs::is_directory(P(path), ec);
}

std::vector<std::string> list_dir(const std::string& path, bool dirs) {
	std::vector<std::string> v;
	std::error_code ec;
	for (fs::directory_iterator it(P(path), ec), end; !ec && it != end; it.increment(ec)) {
		if (it->is_directory(ec) == dirs) v.push_back(it->path().filename().u8string());
	}
	std::sort(v.begin(), v.end());
	return v;
}

std::string join(const std::string& a, const std::string& b) {
	if (a.empty()) return b;
	return (P(a) / P(b)).u8string();
}

const char* library_suffix() {
#if defined(_WIN32)
	return ".dll";
#else
	return ".dylib";
#endif
}

} // namespace awva_app
