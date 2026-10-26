// GENERATED from app/design/tokens.json by app/tool/gen_tokens.dart: do not edit.
// SPDX-FileCopyrightText: 2026 Sorbet AI
// SPDX-License-Identifier: Apache-2.0
//
// The awva design tokens (app/DESIGN.md) for the plugins' editor; the app's theme has the same.
#pragma once

#include <cstdint>

namespace awva_tokens {

struct Colors { // 0xAARRGGBB
	uint32_t bg0, bg1, bg2, bg3, hairline, text1, text2, text3, sage, blue, butter, onSage, error, shadow;
	bool dark;
};
inline constexpr Colors kLight = {0xFFF6F1E7u, 0xFFFBF8F2u, 0xFFEEE7DAu, 0xFFE4DCCBu, 0xFFDDD4C2u, 0xFF2A2925u, 0xFF66625Au, 0xFF8E897Eu, 0xFF5F805Bu, 0xFF5E7D95u, 0xFF9A7B28u, 0xFFFBF8F2u, 0xFFB4483Cu, 0x2E3A3020u, false};
inline constexpr Colors kDark = {0xFF141613u, 0xFF1B1D1Au, 0xFF232622u, 0xFF2D312Cu, 0xFF343832u, 0xFFF3EEE3u, 0xFFA7A396u, 0xFF6D6A61u, 0xFF9EB69Bu, 0xFF7896ADu, 0xFFDCC584u, 0xFF141613u, 0xFFE5866Fu, 0x8C000000u, true};

inline constexpr float s1 = 4.0f;
inline constexpr float s2 = 8.0f;
inline constexpr float s3 = 12.0f;
inline constexpr float s4 = 16.0f;
inline constexpr float s5 = 24.0f;
inline constexpr float s6 = 32.0f;
inline constexpr float s7 = 48.0f;
inline constexpr float header = 56.0f;
inline constexpr float footer = 32.0f;
inline constexpr float row = 32.0f;
inline constexpr float radiusSmall = 6.0f;
inline constexpr float radius = 10.0f;
inline constexpr float radiusLarge = 14.0f;
inline constexpr float knob = 76.0f;
inline constexpr float knobStroke = 5.0f;
inline constexpr float sliderStroke = 4.0f;
inline constexpr float meterStroke = 2.0f;

struct Type {
	float size, line;
	int weight;
	float spacing;
};
inline constexpr const char* kFamily = "DM Sans";
inline constexpr Type typeMicro = {11.0f, 14.0f, 600, 0.6f};
inline constexpr Type typeSmall = {12.0f, 16.0f, 400, 0.0f};
inline constexpr Type typeBody = {13.0f, 18.0f, 400, 0.0f};
inline constexpr Type typeBodyStrong = {13.0f, 18.0f, 500, 0.0f};
inline constexpr Type typeValue = {12.0f, 16.0f, 500, 0.0f};
inline constexpr Type typeTitle = {15.0f, 20.0f, 600, 0.0f};
inline constexpr Type typeSound = {26.0f, 32.0f, 500, -0.3f};

inline constexpr int kEditorWidth = 760;
inline constexpr int kEditorHeight = 480;
inline constexpr int kEditorMinWidth = 640;
inline constexpr int kEditorMinHeight = 400;
inline constexpr int kEditorLargeWidth = 1040;
inline constexpr int kEditorLargeHeight = 640;

} // namespace awva_tokens
