#!/usr/bin/env python3
# Parses the Windows resource-compiler STRINGTABLE files under res/lang_*.rc2 (one per
# language, keyed by the compute_crc16() of the original English string - see
# source/localization.hpp) into a generated C++ source providing the same lookup on Linux,
# where there is no resource compiler or MUI language fallback to rely on.
#
# Every res/lang_*.rc2 STRINGTABLE entry is expected on its own line, in the form:
#   <decimal id> "<RC-escaped string>"
# RC escaping recognizes '""' as an embedded quote and '\n'/'\t'/'\\'/'\"' as in C.

import re
import sys
from pathlib import Path

LINE_RE = re.compile(r'^\s*(\d+)\s+"(.*)"\s*$')


def decode_rc_string(s: str) -> str:
	out = []
	i, n = 0, len(s)
	while i < n:
		c = s[i]
		if c == '"' and i + 1 < n and s[i + 1] == '"':
			out.append('"')
			i += 2
			continue
		if c == '\\' and i + 1 < n and s[i + 1] in 'ntr\\"':
			out.append({'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"'}[s[i + 1]])
			i += 2
			continue
		out.append(c)
		i += 1
	return ''.join(out)


def encode_cpp_string(s: str) -> str:
	out = ['"']
	for ch in s:
		if ch == '\\':
			out.append('\\\\')
		elif ch == '"':
			out.append('\\"')
		elif ch == '\n':
			out.append('\\n')
		elif ch == '\t':
			out.append('\\t')
		elif ch == '\r':
			out.append('\\r')
		else:
			out.append(ch)
	out.append('"')
	return ''.join(out)


def parse_rc2(path: Path):
	entries = []
	for line in path.read_text(encoding='utf-8').splitlines():
		match = LINE_RE.match(line)
		if match is None:
			continue
		entries.append((int(match.group(1)), decode_rc_string(match.group(2))))
	return entries


def main():
	if len(sys.argv) != 4:
		print(f"Usage: {sys.argv[0]} <res dir> <output .hpp> <output .cpp>", file=sys.stderr)
		return 1

	res_dir, out_header, out_source = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
	rc2_files = sorted(res_dir.glob('lang_*.rc2'))
	if not rc2_files:
		print(f"No res/lang_*.rc2 files found in {res_dir}", file=sys.stderr)
		return 1

	out_header.write_text(
		"#pragma once\n"
		"#include <string>\n"
		"#include <vector>\n"
		"\n"
		"namespace reshade::resources\n"
		"{\n"
		"\tconst char *find_localized_string_linux(const std::string &language, unsigned short id);\n"
		"\tstd::vector<std::string> get_languages_linux();\n"
		"}\n")

	cpp = []
	cpp.append("#include \"localization_linux.hpp\"\n#include <cstring>\n#include <unordered_map>\n\nnamespace\n{\n")

	languages = []
	for rc2 in rc2_files:
		code = rc2.stem[len('lang_'):]
		identifier = re.sub(r'[^A-Za-z0-9]', '_', code)
		languages.append((code, identifier))

		entries = parse_rc2(rc2)
		cpp.append(f"\tconst std::unordered_map<unsigned short, const char *> s_language_{identifier} = {{\n")
		for id_value, text in entries:
			cpp.append(f"\t\t{{ {id_value}, {encode_cpp_string(text)} }},\n")
		cpp.append("\t};\n")

	cpp.append("\n\tstruct language_table { const char *code; const std::unordered_map<unsigned short, const char *> *strings; };\n")
	cpp.append("\tconst language_table s_language_tables[] = {\n")
	for code, identifier in languages:
		cpp.append(f"\t\t{{ \"{code}\", &s_language_{identifier} }},\n")
	cpp.append("\t};\n}\n\n")

	cpp.append(
		"const char *reshade::resources::find_localized_string_linux(const std::string &language, unsigned short id)\n"
		"{\n"
		"\tfor (const language_table &entry : s_language_tables)\n"
		"\t\tif (language == entry.code)\n"
		"\t\t{\n"
		"\t\t\tconst auto it = entry.strings->find(id);\n"
		"\t\t\treturn it != entry.strings->end() ? it->second : nullptr;\n"
		"\t\t}\n"
		"\treturn nullptr;\n"
		"}\n\n"
		"std::vector<std::string> reshade::resources::get_languages_linux()\n"
		"{\n"
		"\tstd::vector<std::string> result;\n"
		"\tresult.reserve(std::size(s_language_tables));\n"
		"\tfor (const language_table &entry : s_language_tables)\n"
		"\t\tresult.push_back(entry.code);\n"
		"\treturn result;\n"
		"}\n")

	out_source.write_text(''.join(cpp))
	return 0


if __name__ == '__main__':
	sys.exit(main())
