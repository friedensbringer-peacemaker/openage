// Copyright 2026 the openage authors. See copying.md for legal info.

#pragma once

#include <algorithm>
#include <string>


namespace openage::renderer::opengl {

/**
 * Translate a desktop GLSL shader source (written for `#version 330`) into a
 * GLSL ES 3.x source for an OpenGL ES 3.x context.
 *
 * The shader sources stay desktop GLSL, only the header is exchanged:
 * the `#version` line is replaced by `#version <major><minor>0 es` (e.g.
 * `320 es` on an OpenGL ES 3.2 context) and default precision qualifiers are
 * inserted after it (and after any `#extension` lines following it), because
 * GLSL ES fragment shaders have no default float precision and integer/array
 * samplers have no default precision in any stage. A source without
 * `#version` line gets the header prepended.
 *
 * Everything else must already be valid in both dialects (no implicit
 * int -> float conversions, no non-constant global initializers, explicit
 * output locations when a fragment shader has several outputs).
 *
 * This header has no dependencies, so it can be tested without a GL context.
 *
 * @param src Desktop GLSL source.
 * @param major Major version of the OpenGL ES context (3).
 * @param minor Minor version of the OpenGL ES context (0, 1 or 2).
 *
 * @return GLSL ES source.
 */
inline std::string glsl_to_gles(const std::string &src, int major, int minor) {
	const std::string header = "#version " + std::to_string(major) + std::to_string(minor) + "0 es\n";
	const std::string precision =
		"precision highp float;\n"
		"precision highp int;\n"
		"precision highp sampler2D;\n"
		"precision highp sampler2DArray;\n"
		"precision highp sampler3D;\n"
		"precision highp samplerCube;\n"
		"precision highp isampler2D;\n"
		"precision highp usampler2D;\n"
		"precision highp isampler2DArray;\n"
		"precision highp usampler2DArray;\n";

	// returns the line starting at pos with leading blanks removed
	auto trimmed_line = [&](size_t pos, size_t &line_end) {
		line_end = src.find('\n', pos);
		if (line_end == std::string::npos) {
			line_end = src.size();
		}
		size_t first = src.find_first_not_of(" \t\r", pos);
		if (first == std::string::npos or first >= line_end) {
			return std::string{};
		}
		return src.substr(first, line_end - first);
	};

	// find the #version line, only blank lines may precede it
	size_t pos = 0;
	size_t line_end = 0;
	bool found_version = false;
	size_t version_begin = 0;
	while (pos < src.size()) {
		std::string line = trimmed_line(pos, line_end);
		if (line.rfind("#version", 0) == 0) {
			found_version = true;
			version_begin = pos;
			break;
		}
		if (not line.empty()) {
			break;
		}
		pos = line_end + 1;
	}

	if (not found_version) {
		return header + precision + "#line 1\n" + src;
	}

	// skip #extension directives directly after #version,
	// they must come before the first declaration
	size_t rest = std::min(line_end + 1, src.size());
	std::string extensions;
	size_t line_no = 2; // line number of the line at rest (1-based)
	while (rest < src.size()) {
		size_t ext_end;
		std::string line = trimmed_line(rest, ext_end);
		if (line.rfind("#extension", 0) != 0) {
			break;
		}
		extensions += line + "\n";
		rest = std::min(ext_end + 1, src.size());
		line_no += 1;
	}

	// count the lines before #version, so that compiler messages keep
	// the line numbers of the original file
	for (size_t i = 0; i < version_begin; ++i) {
		if (src[i] == '\n') {
			line_no += 1;
		}
	}

	return header + extensions + precision
	       + "#line " + std::to_string(line_no) + "\n"
	       + src.substr(rest);
}

} // namespace openage::renderer::opengl
