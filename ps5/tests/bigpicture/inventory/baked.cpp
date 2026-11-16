// Crux2 - the settings inventory: bp_settings_text.h as the compiler reads it, printed as JSON, for
// settings-text.py check --baked (the baked words must be the code's in phase 1 of the revamp).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "bp_settings_text.h"

#include <cstdio>
#include <cstring>
#include <string>

static std::string json(const char* text)
{
	std::string out = "\"";
	for (const char* c = text; *c; c++)
	{
		const unsigned char u = static_cast<unsigned char>(*c);
		if (u == '"' || u == '\\')
			out += '\\';
		if (u < 0x20)
		{
			char escaped[8];
			std::snprintf(escaped, sizeof(escaped), "\\u%04x", u);
			out += escaped;
			continue;
		}
		out += *c;
	}
	return out + "\"";
}

int main()
{
	std::printf("[\n");
	bool first = true;
	for (const bp::SettingsText& t : bp::settingsText)
	{
		std::string choices;
		for (const char* c = t.choices; *c; c += std::strlen(c) + 1)
			choices += (choices.empty() ? "" : ", ") + json(c);
		std::printf("%s{\"id\": %s, \"label\": %s, \"desc\": %s, \"choices\": [%s]}", first ? "" : ",\n", json(t.id).c_str(), json(t.label).c_str(),
			json(t.desc).c_str(), choices.c_str());
		first = false;
	}
	std::printf("\n]\n");
	return 0;
}
