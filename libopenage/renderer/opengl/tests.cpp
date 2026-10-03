// Copyright 2026 the openage authors. See copying.md for legal info.

#include <string>
#include <vector>

#include "testing/testing.h"

#include "renderer/opengl/glsl_es.h"


namespace openage::renderer::opengl::tests {

namespace {

size_t count(const std::string &haystack, const std::string &needle) {
	size_t n = 0;
	for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) {
		n += 1;
	}
	return n;
}

} // namespace

/**
 * Desktop GLSL -> GLSL ES header translation, swept over all supported
 * OpenGL ES versions and the source layouts used in openage.
 */
void glsl_es() {
	const std::vector<std::pair<int, int>> versions{{3, 0}, {3, 1}, {3, 2}};

	struct source_case {
		std::string src;
		std::string body;     // must survive unchanged
		std::string ext;      // #extension line that must stay before precision
		size_t line_of_body;  // expected #line value
	};

	const std::vector<source_case> cases{
		// asset shader file
		{"#version 330\n\nin vec2 tex_pos;\n", "\nin vec2 tex_pos;\n", "", 2},
		// "core" suffix and CRLF line endings
		{"#version 330 core\r\nout vec4 col;\r\n", "out vec4 col;\r\n", "", 2},
		// inline shader in a raw string literal (leading newline, indentation)
		{"\n  #version 330\nlayout(location=0) in vec2 position;\n", "layout(location=0) in vec2 position;\n", "", 3},
		// extension directive must stay before the precision statements
		{"#version 330\n#extension GL_EXT_foo : enable\nvoid main() {}\n", "void main() {}\n", "#extension GL_EXT_foo : enable\n", 3},
		// no #version at all
		{"void main() {}\n", "void main() {}\n", "", 1},
		// #version as last line without newline
		{"#version 330", "", "", 2},
	};

	for (const auto &[major, minor] : versions) {
		const std::string header = "#version " + std::to_string(major) + std::to_string(minor) + "0 es\n";

		for (const auto &c : cases) {
			const std::string out = glsl_to_gles(c.src, major, minor);

			// exactly one version line, and it is the first line
			(out.rfind(header, 0) == 0) or TESTFAILMSG("missing ES header for " << major << "." << minor << ":\n"
			                                                                    << out);
			(count(out, "#version") == 1) or TESTFAILMSG("duplicate #version:\n"
			                                             << out);

			// float precision declared, the desktop version is gone
			(count(out, "precision highp float;") == 1) or TESTFAILMSG("missing float precision:\n"
			                                                           << out);
			(out.find("usampler2D;") != std::string::npos) or TESTFAILMSG("missing usampler2D precision:\n"
			                                                              << out);
			(out.find("330") == std::string::npos) or TESTFAILMSG("desktop version left over:\n"
			                                                      << out);

			// shader body is kept and comes after the precision block
			size_t prec = out.find("precision highp float;");
			size_t body = out.rfind(c.body);
			(body != std::string::npos and body > prec) or TESTFAILMSG("body lost or misplaced:\n"
			                                                          << out);

			// line numbers of compiler messages refer to the original file
			std::string line = "#line " + std::to_string(c.line_of_body) + "\n";
			(out.find(line) != std::string::npos) or TESTFAILMSG("expected '" << line << "' in:\n"
			                                                                   << out);

			if (not c.ext.empty()) {
				size_t ext = out.find(c.ext);
				(ext != std::string::npos and ext < prec) or TESTFAILMSG("#extension after precision:\n"
				                                                         << out);
			}
		}
	}
}

} // namespace openage::renderer::opengl::tests
