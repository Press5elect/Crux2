// Crux2 - text from elsewhere (an encyclopedia) made fit for the interface's font. No console
// and no drawing in it: the PC tests check it.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bp
{
	/// The text without what `shown` says the font has no letter for: a part in brackets that
	/// holds such a letter goes whole ("Tekken 5 (Japanese: 鉄拳5, Hepburn: Tekken Faibu) is" ->
	/// "Tekken 5 is"), and any other such letter goes alone; the spaces left are tidied.
	inline std::string StripUnshown(std::string_view text, const std::function<bool(char32_t)>& shown)
	{
		// UTF-8 into code points, each with where it starts and ends.
		struct Cp
		{
			char32_t c;
			size_t at, len;
		};
		std::vector<Cp> cps;
		for (size_t i = 0; i < text.size();)
		{
			const unsigned char b = static_cast<unsigned char>(text[i]);
			size_t len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
			if (i + len > text.size())
				len = 1;
			char32_t c = len == 1 ? b : (b & (0x7f >> len));
			for (size_t k = 1; k < len; k++)
				c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3f);
			cps.push_back({c, i, len});
			i += len;
		}
		std::vector<bool> keep(cps.size(), true);
		for (size_t i = 0; i < cps.size(); i++)
		{
			if (cps[i].c != '(')
				continue;
			int depth = 0;
			size_t end = i;
			bool foreign = false;
			for (size_t j = i; j < cps.size(); j++)
			{
				depth += cps[j].c == '(' ? 1 : cps[j].c == ')' ? -1 : 0;
				foreign = foreign || !shown(cps[j].c);
				if (depth == 0)
				{
					end = j;
					break;
				}
			}
			if (end > i && foreign)
			{
				for (size_t j = i; j <= end; j++)
					keep[j] = false;
				i = end;
			}
		}
		std::string out;
		for (size_t i = 0; i < cps.size(); i++)
		{
			if (keep[i] && (cps[i].c < 0x80 || shown(cps[i].c)))
				out.append(text.substr(cps[i].at, cps[i].len));
		}
		// Tidy: no double spaces, no space before , . ; :
		std::string tidy;
		for (char c : out)
		{
			if (c == ' ' && (tidy.empty() || tidy.back() == ' '))
				continue;
			if ((c == ',' || c == '.' || c == ';' || c == ':') && !tidy.empty() && tidy.back() == ' ')
				tidy.pop_back();
			tidy += c;
		}
		while (!tidy.empty() && tidy.back() == ' ')
			tidy.pop_back();
		return tidy;
	}
} // namespace bp
