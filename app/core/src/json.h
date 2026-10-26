// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// A small JSON reader (manifests, presets' titles and heroes, the options) and writer helpers.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace awva_app {

struct Json {
	enum Type { Null, Bool, Number, String, Array, Object } type = Null;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<Json> a;
	std::map<std::string, Json> o;

	const Json* get(const std::string& k) const {
		if (type != Object) return nullptr;
		auto it = o.find(k);
		return it == o.end() ? nullptr : &it->second;
	}
	std::string str(const std::string& k, const std::string& def = "") const {
		const Json* v = get(k);
		return v && v->type == String ? v->s : def;
	}
	bool flag(const std::string& k, bool def) const {
		const Json* v = get(k);
		return v && v->type == Bool ? v->b : def;
	}
	double num(const std::string& k, double def) const {
		const Json* v = get(k);
		return v && v->type == Number ? v->n : def;
	}
	bool has(const std::string& k) const { return get(k) != nullptr; }

	// false: not JSON (out unchanged in meaning)
	static bool parse(const std::string& text, Json* out) {
		size_t i = 0;
		if (!value(text, i, out, 0)) return false;
		ws(text, i);
		return i == text.size();
	}

private:
	static void ws(const std::string& t, size_t& i) {
		while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++;
	}
	static bool lit(const std::string& t, size_t& i, const char* w) {
		size_t k = 0;
		while (w[k]) {
			if (i + k >= t.size() || t[i + k] != w[k]) return false;
			k++;
		}
		i += k;
		return true;
	}
	static void utf8(std::string& o, unsigned c) {
		if (c < 0x80) o += static_cast<char>(c);
		else if (c < 0x800) {
			o += static_cast<char>(0xc0 | (c >> 6));
			o += static_cast<char>(0x80 | (c & 0x3f));
		} else if (c < 0x10000) {
			o += static_cast<char>(0xe0 | (c >> 12));
			o += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
			o += static_cast<char>(0x80 | (c & 0x3f));
		} else {
			o += static_cast<char>(0xf0 | (c >> 18));
			o += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
			o += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
			o += static_cast<char>(0x80 | (c & 0x3f));
		}
	}
	static bool hex4(const std::string& t, size_t& i, unsigned* c) {
		if (i + 4 > t.size()) return false;
		*c = static_cast<unsigned>(std::strtoul(t.substr(i, 4).c_str(), nullptr, 16));
		i += 4;
		return true;
	}
	static bool string(const std::string& t, size_t& i, std::string* out) {
		if (i >= t.size() || t[i] != '"') return false;
		i++;
		out->clear();
		while (i < t.size() && t[i] != '"') {
			char c = t[i++];
			if (c != '\\') {
				*out += c;
				continue;
			}
			if (i >= t.size()) return false;
			c = t[i++];
			switch (c) {
			case 'n': *out += '\n'; break;
			case 't': *out += '\t'; break;
			case 'r': *out += '\r'; break;
			case 'b': *out += '\b'; break;
			case 'f': *out += '\f'; break;
			case 'u': {
				unsigned u;
				if (!hex4(t, i, &u)) return false;
				if (u >= 0xd800 && u < 0xdc00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
					i += 2;
					unsigned lo;
					if (!hex4(t, i, &lo)) return false;
					u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
				}
				utf8(*out, u);
				break;
			}
			default: *out += c; break;
			}
		}
		if (i >= t.size()) return false;
		i++;
		return true;
	}
	static bool value(const std::string& t, size_t& i, Json* out, int depth) {
		if (depth > 64) return false;
		ws(t, i);
		if (i >= t.size()) return false;
		const char c = t[i];
		if (c == '{') {
			out->type = Object;
			i++;
			ws(t, i);
			if (i < t.size() && t[i] == '}') return ++i, true;
			for (;;) {
				ws(t, i);
				std::string k;
				if (!string(t, i, &k)) return false;
				ws(t, i);
				if (i >= t.size() || t[i] != ':') return false;
				i++;
				Json v;
				if (!value(t, i, &v, depth + 1)) return false;
				out->o[k] = std::move(v);
				ws(t, i);
				if (i < t.size() && t[i] == ',') {
					i++;
					continue;
				}
				if (i < t.size() && t[i] == '}') return ++i, true;
				return false;
			}
		}
		if (c == '[') {
			out->type = Array;
			i++;
			ws(t, i);
			if (i < t.size() && t[i] == ']') return ++i, true;
			for (;;) {
				Json v;
				if (!value(t, i, &v, depth + 1)) return false;
				out->a.push_back(std::move(v));
				ws(t, i);
				if (i < t.size() && t[i] == ',') {
					i++;
					continue;
				}
				if (i < t.size() && t[i] == ']') return ++i, true;
				return false;
			}
		}
		if (c == '"') {
			out->type = String;
			return string(t, i, &out->s);
		}
		if (lit(t, i, "true")) return out->type = Bool, out->b = true, true;
		if (lit(t, i, "false")) return out->type = Bool, out->b = false, true;
		if (lit(t, i, "null")) return out->type = Null, true;
		const char* p = t.c_str() + i;
		char* end = nullptr;
		const double d = std::strtod(p, &end);
		if (end == p) return false;
		i += static_cast<size_t>(end - p);
		out->type = Number;
		out->n = d;
		return true;
	}
};

// writing
inline std::string jstr(const std::string& s) {
	std::string o = "\"";
	for (const unsigned char c : s) {
		switch (c) {
		case '"': o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if (c < 0x20) {
				char b[8];
				std::snprintf(b, sizeof b, "\\u%04x", c);
				o += b;
			} else {
				o += static_cast<char>(c);
			}
		}
	}
	return o + "\"";
}
inline std::string jnum(double v) {
	char b[32];
	std::snprintf(b, sizeof b, "%.9g", v);
	std::string s = b;
	if (s == "nan" || s == "-nan" || s == "inf" || s == "-inf") return "null";
	return s;
}

} // namespace awva_app
