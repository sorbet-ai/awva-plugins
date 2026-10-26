// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// A stereo float-32 WAV writer (the probe tap and offline renders: the exact output samples).
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace awva_app {

class WavWriter {
public:
	bool open(const std::string& path, int rate) {
		close();
#if defined(_WIN32)
		std::wstring w(path.begin(), path.end());
		f_ = _wfopen(w.c_str(), L"wb");
#else
		f_ = std::fopen(path.c_str(), "wb");
#endif
		if (!f_) return false;
		rate_ = rate;
		frames_ = 0;
		header();
		return true;
	}
	void write(const float* l, const float* r, uint32_t n) {
		if (!f_) return;
		buf_.resize(static_cast<size_t>(n) * 2);
		for (uint32_t i = 0; i < n; i++) {
			buf_[2 * i] = l[i];
			buf_[2 * i + 1] = r[i];
		}
		std::fwrite(buf_.data(), sizeof(float), buf_.size(), f_);
		frames_ += n;
	}
	void close() {
		if (!f_) return;
		std::fseek(f_, 0, SEEK_SET);
		header();
		std::fclose(f_);
		f_ = nullptr;
	}
	~WavWriter() { close(); }

private:
	void u32(uint32_t v) { std::fwrite(&v, 4, 1, f_); }
	void u16(uint16_t v) { std::fwrite(&v, 2, 1, f_); }
	void header() {
		const uint32_t data = static_cast<uint32_t>(frames_ * 8);
		std::fwrite("RIFF", 1, 4, f_);
		u32(36 + data);
		std::fwrite("WAVEfmt ", 1, 8, f_);
		u32(16);
		u16(3); // IEEE float
		u16(2);
		u32(static_cast<uint32_t>(rate_));
		u32(static_cast<uint32_t>(rate_) * 8);
		u16(8);
		u16(32);
		std::fwrite("data", 1, 4, f_);
		u32(data);
	}
	FILE* f_ = nullptr;
	int rate_ = 48000;
	uint64_t frames_ = 0;
	std::vector<float> buf_;
};

} // namespace awva_app
