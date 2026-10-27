// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The few things the core needs from the OS besides audio and MIDI: a monotonic clock, paths,
// files, and lock-free rings between threads.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace awva_app {

uint64_t now_ns(); // a monotonic clock, the one MIDI and audio timestamps are on

std::string core_dir();       // the folder the core library is in
std::string app_data_dir();   // %APPDATA%\awva, ~/Library/Application Support/awva
std::string cache_dir();      // %LOCALAPPDATA%\awva\cache, ~/Library/Caches/awva
bool read_file(const std::string& path, std::string* out, size_t limit = 1u << 22);
bool write_file(const std::string& path, const std::string& text);
bool copy_file(const std::string& from, const std::string& to);
bool make_dirs(const std::string& path);
std::vector<std::string> list_dir(const std::string& path, bool dirs); // names, sorted
std::string join(const std::string& a, const std::string& b);
const char* library_suffix(); // ".dll", ".dylib"
std::string file_stamp(const std::string& dir); // every file under dir: its name, size and time

// a single-producer single-consumer ring of T (N a power of 2)
template <class T, size_t N> class Ring {
public:
	bool push(const T& v) {
		const size_t w = w_.load(std::memory_order_relaxed);
		if (w - r_.load(std::memory_order_acquire) == N) return false;
		buf_[w & (N - 1)] = v;
		w_.store(w + 1, std::memory_order_release);
		return true;
	}
	bool pop(T* v) {
		const size_t r = r_.load(std::memory_order_relaxed);
		if (r == w_.load(std::memory_order_acquire)) return false;
		*v = buf_[r & (N - 1)];
		r_.store(r + 1, std::memory_order_release);
		return true;
	}

private:
	T buf_[N];
	std::atomic<size_t> w_{0}, r_{0};
};

} // namespace awva_app
