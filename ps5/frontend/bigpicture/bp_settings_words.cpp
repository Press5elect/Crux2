// Crux2 - the interface: every settings row's words and place, from settings-text.yaml.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The pages of bp_options.cpp and the Backend's are made with the code's words; settings-text.yaml
// (baked into bp_settings_text.h by ps5/tools/settings-text.py) is where the words shown are kept,
// and where each row's area, section and level are. This puts them on the rows: each row is found in
// the table by its id, as settings-inventory.py makes it (the setting's section.key, or the kind and
// the code's label for an action or a line), on its page and under its heading of today. A row whose
// words come from data (a game's patches, PCSX2's controller tables) keeps its label; a row the table
// has no entry for (a fourth controller's, a game's own cheat) keeps the code's words and is placed
// where the row before it is.

#include "bp_internal.h"
#include "bp_settings_text.h"

#include <cstring>
#include <map>

namespace bp
{

namespace
{

const char* kindName(Option::Kind kind)
{
	switch (kind)
	{
		case Option::Heading: return "heading";
		case Option::Bool: return "toggle";
		case Option::IntList: return "intlist";
		case Option::FloatList: return "floatlist";
		case Option::StringList: return "stringlist";
		case Option::IntRange: return "intrange";
		case Option::FloatRange: return "floatrange";
		case Option::Combo: return "combo";
		case Option::Action: return "action";
		case Option::Info: return "info";
		case Option::Bind: return "bind";
	}
	return "info";
}

bool isSetting(Option::Kind kind)
{
	return kind != Option::Heading && kind != Option::Action && kind != Option::Info;
}

// The table's rows by their id without the place after "@" (and a "#n" after it).
const std::multimap<std::string, int>& byBase()
{
	static const std::multimap<std::string, int> index = [] {
		std::multimap<std::string, int> out;
		for (int i = 0; i < static_cast<int>(sizeof(settingsText) / sizeof(settingsText[0])); i++)
		{
			std::string id = settingsText[i].id;
			const size_t at = id.find_first_of("@#");
			if (at != std::string::npos)
				id.resize(at);
			out.emplace(std::move(id), i);
		}
		return out;
	}();
	return index;
}

int find(const std::string& base, const std::string& page, const std::string& heading)
{
	const auto [from, to] = byBase().equal_range(base);
	int samePage = -1, any = -1;
	for (auto it = from; it != to; ++it)
	{
		const SettingsText& t = settingsText[it->second];
		if (page == t.page && heading == t.heading)
			return it->second;
		if (samePage < 0 && page == t.page)
			samePage = it->second;
		if (any < 0)
			any = it->second;
	}
	return samePage >= 0 ? samePage : any;
}

void put(Option& o, int row)
{
	const SettingsText& t = settingsText[row];
	o.textRow = row;
	if (!t.dynamic && o.label != t.label)
	{
		o.codeLabel = o.label;
		o.label = t.label;
	}
	o.desc = t.desc;
	if (t.choices[0] != '\0' && !o.names.empty())
	{
		std::vector<std::string> names;
		for (const char* c = t.choices; *c; c += std::strlen(c) + 1)
			names.emplace_back(c);
		if (names.size() == o.names.size())
			o.names = std::move(names);
	}
}

} // namespace

std::string settingsSlug(const std::string& text)
{
	std::string out;
	bool gap = false;
	for (const char c : text)
	{
		const char l = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9'))
		{
			if (gap && !out.empty())
				out += '-';
			gap = false;
			out += l;
		}
		else
		{
			gap = true;
		}
	}
	if (out.size() > 56)
		out.resize(56);
	while (!out.empty() && out.back() == '-')
		out.pop_back();
	return out.empty() ? std::string("row") : out;
}

std::string settingsBaseId(const Option& o)
{
	if (isSetting(o.kind) && !o.key.empty())
		return o.section + "." + o.key;
	return std::string(kindName(o.kind)) + ":" + settingsSlug(o.codeLabel.empty() ? o.label : o.codeLabel);
}

void applySettingsText(Page& page)
{
	std::string heading;
	int port = 1; // the controller port the rows under the heading are of
	for (Option& o : page.rows)
	{
		if (o.kind == Option::Heading)
		{
			heading = o.label;
			port = 1;
			continue;
		}
		if (o.section.size() == 4 && o.section.compare(0, 3, "Pad") == 0 && o.section[3] >= '1' && o.section[3] <= '8')
			port = o.section[3] - '0';
		if (o.textRow >= 0)
			continue;
		const std::string base = settingsBaseId(o);
		int row = find(base, page.name, heading);
		// A controller of a multitap port has the first port's words.
		if (row < 0 && base.size() > 5 && base.compare(0, 3, "Pad") == 0 && base[3] >= '2' && base[3] <= '8' && base[4] == '.')
			row = find("Pad1" + base.substr(4), page.name, heading);
		if (row >= 0)
		{
			// A port's action (Map to ...) has the first port's words, but its own id and its own description.
			const bool portAction = o.kind == Option::Action && port >= 2 && o.codeLabel.empty();
			const std::string code = portAction ? o.label : std::string();
			const std::string codeDesc = portAction ? o.desc : std::string();
			put(o, row);
			if (portAction)
			{
				o.codeLabel = (o.codeLabel.empty() ? code : o.codeLabel) + " port " + std::to_string(port);
				if (!codeDesc.empty())
					o.desc = codeDesc;
			}
		}
	}
}

void applySettingsText(std::vector<Page>& pages)
{
	for (Page& page : pages)
		applySettingsText(page);
}

const SettingsText* settingsTextOf(const Option& o)
{
	constexpr int count = static_cast<int>(sizeof(settingsText) / sizeof(settingsText[0]));
	return (o.textRow >= 0 && o.textRow < count) ? &settingsText[o.textRow] : nullptr;
}

std::string settingsIdOf(const Option& o)
{
	if (const SettingsText* t = settingsTextOf(o))
	{
		// A row of ports 2 to 8 has the first port's words, not its id.
		const bool ofPort = (o.section.size() == 4 && o.section.compare(0, 3, "Pad") == 0 && o.section[3] >= '2' && o.section[3] <= '8') ||
							(o.kind == Option::Action && o.codeLabel.size() > 7 && o.codeLabel.find(" port ", o.codeLabel.size() - 7) != std::string::npos);
		if (ofPort)
		{
			const std::string base = settingsBaseId(o);
			std::string id = t->id;
			const size_t at = id.find_first_of("@#");
			if (at != std::string::npos)
				id.resize(at);
			if (id != base)
				return base;
		}
		return t->id;
	}
	return settingsBaseId(o);
}

} // namespace bp
