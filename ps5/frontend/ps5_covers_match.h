// Crux2 - which libretro box art belongs to which game.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Pure logic: no PCSX2, no files and no network in here (ps5/tests/online builds it for the PC
// and runs it over the real list of names).
//
// libretro's pictures (github.com/libretro-thumbnails/Sony_-_PlayStation_2, Named_Boxarts) are
// named the way the Redump database names a disc: "Title, The - Subtitle (Region) (Languages)
// (Disc 1) (v2.00).png", with "_" where the title has one of &*/:`<>?\| . A game of the list is
// known by its file's name, by the title PCSX2's database gives its serial (no tags, ": " where
// Redump writes " - ", sometimes another spelling, Japanese for Japanese games with an English or
// romaji one beside it) and by its region. The match goes from the surest to the least sure:
//   1. the file's name is a picture's name;
//   2. the same title once both are brought to one form (case, accents, punctuation, "The" at
//      the end, "&" and "and", roman numerals);
//   3. one of them has a subtitle the other leaves out, and no other title of the game's region
//      could be meant;
//   4. a letter or two of difference in a long title, the numbers in it the same, and again
//      nothing else it could be.
// Among the pictures of one title the game's own region comes first, then its disc; a picture of
// another region is only taken for the same title (1 and 2). When nothing is clearly the same
// title there is no match: an empty tile is better than another game's cover.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace PS5Covers
{
	// Where a disc was sold, as far as covers differ.
	enum RegionBit : uint32_t
	{
		R_USA = 1u << 0,
		R_EUROPE = 1u << 1,
		R_JAPAN = 1u << 2,
		R_KOREA = 1u << 3,
		R_ASIA = 1u << 4, // Asia, China, Taiwan, Hong Kong
		R_AUSTRALIA = 1u << 5,
		R_UK = 1u << 6,
		R_FRANCE = 1u << 7,
		R_GERMANY = 1u << 8,
		R_ITALY = 1u << 9,
		R_SPAIN = 1u << 10,
		R_NETHERLANDS = 1u << 11,
		R_NORDIC = 1u << 12, // Scandinavia, Sweden, Norway, Denmark, Finland
		R_RUSSIA = 1u << 13,
		R_POLAND = 1u << 14,
		R_PORTUGAL = 1u << 15,
		R_OTHER_EUROPE = 1u << 16, // Greece, Belgium, Switzerland, Croatia, Austria
		R_AMERICAS = 1u << 17, // Canada, Brazil, Latin America
		R_INDIA = 1u << 18,
		R_WORLD = 1u << 19,

		R_ANY_EUROPE = R_EUROPE | R_AUSTRALIA | R_UK | R_FRANCE | R_GERMANY | R_ITALY | R_SPAIN | R_NETHERLANDS | R_NORDIC | R_RUSSIA |
					   R_POLAND | R_PORTUGAL | R_OTHER_EUROPE | R_INDIA,
		R_ANY_ASIA = R_JAPAN | R_KOREA | R_ASIA,
	};

	namespace detail
	{
		inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
		inline bool IsAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
		inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }

		inline bool EqualNoCase(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); i++)
			{
				if (Lower(a[i]) != Lower(b[i]))
					return false;
			}
			return true;
		}

		inline bool LessNoCase(std::string_view a, std::string_view b)
		{
			const size_t n = std::min(a.size(), b.size());
			for (size_t i = 0; i < n; i++)
			{
				const char x = Lower(a[i]);
				const char y = Lower(b[i]);
				if (x != y)
					return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
			}
			return a.size() < b.size();
		}

		inline std::string_view Trim(std::string_view text)
		{
			while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
				text.remove_prefix(1);
			while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
				text.remove_suffix(1);
			return text;
		}

		// The bit of one word of a "(Region, Region)" tag; 0 when the word is not a region.
		inline uint32_t RegionWord(std::string_view word)
		{
			static const struct
			{
				const char* name;
				uint32_t bit;
			} words[] = {
				{"USA", R_USA},
				{"Europe", R_EUROPE},
				{"Japan", R_JAPAN},
				{"Korea", R_KOREA},
				{"Asia", R_ASIA},
				{"China", R_ASIA},
				{"Taiwan", R_ASIA},
				{"Hong Kong", R_ASIA},
				{"Australia", R_AUSTRALIA},
				{"UK", R_UK},
				{"France", R_FRANCE},
				{"Germany", R_GERMANY},
				{"Italy", R_ITALY},
				{"Spain", R_SPAIN},
				{"Netherlands", R_NETHERLANDS},
				{"Scandinavia", R_NORDIC},
				{"Sweden", R_NORDIC},
				{"Norway", R_NORDIC},
				{"Denmark", R_NORDIC},
				{"Finland", R_NORDIC},
				{"Russia", R_RUSSIA},
				{"Poland", R_POLAND},
				{"Portugal", R_PORTUGAL},
				{"Greece", R_OTHER_EUROPE},
				{"Belgium", R_OTHER_EUROPE},
				{"Switzerland", R_OTHER_EUROPE},
				{"Croatia", R_OTHER_EUROPE},
				{"Austria", R_OTHER_EUROPE},
				{"Canada", R_AMERICAS},
				{"Brazil", R_AMERICAS},
				{"Latin America", R_AMERICAS},
				{"India", R_INDIA},
				{"World", R_WORLD},
			};
			for (const auto& entry : words)
			{
				if (word == entry.name)
					return entry.bit;
			}
			return 0;
		}

		// "(USA)", "(Europe, Australia)": every word a region. 0 for any other tag.
		inline uint32_t RegionTag(std::string_view tag)
		{
			uint32_t bits = 0;
			while (!tag.empty())
			{
				const size_t comma = tag.find(',');
				const uint32_t bit = RegionWord(Trim(tag.substr(0, comma)));
				if (bit == 0)
					return 0;
				bits |= bit;
				tag = (comma == std::string_view::npos) ? std::string_view() : tag.substr(comma + 1);
			}
			return bits;
		}

		// "(En,Fr,De)": two-letter codes, the first letter a capital. A hash of the tag when it is
		// one (never 0), 0 when it is not; the hash only says whether two names have the same list.
		inline uint32_t LanguageTag(std::string_view tag, bool* english)
		{
			if (tag.size() < 2 || (tag.size() + 1) % 3 != 0)
				return 0;
			bool has_english = false;
			for (size_t i = 0; i < tag.size(); i += 3)
			{
				if (!(tag[i] >= 'A' && tag[i] <= 'Z') || !(tag[i + 1] >= 'a' && tag[i + 1] <= 'z'))
					return 0;
				if (i + 2 < tag.size() && tag[i + 2] != ',')
					return 0;
				if (tag[i] == 'E' && tag[i + 1] == 'n')
					has_english = true;
			}
			uint32_t hash = 2166136261u;
			for (const char c : tag)
				hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
			if (english)
				*english = has_english;
			return hash ? hash : 1;
		}

		// Whether a "(Lang,Lang)" tag names the language with this code ("Fr").
		inline bool TagHasLanguage(std::string_view tag, std::string_view code)
		{
			for (size_t i = 0; i + 1 < tag.size(); i += 3)
			{
				if (tag.substr(i, 2) == code)
					return true;
			}
			return false;
		}

		// "Disc 2", "Disc 2 of 3", "disc2of3", "ディスク2/3": the disc's number, 0 when it is none.
		inline int DiscNumber(std::string_view tag)
		{
			static constexpr std::string_view katakana = "\xE3\x83\x87\xE3\x82\xA3\xE3\x82\xB9\xE3\x82\xAF"; // ディスク
			size_t at;
			if (tag.size() >= 4 && EqualNoCase(tag.substr(0, 4), "disc"))
				at = 4;
			else if (tag.substr(0, katakana.size()) == katakana)
				at = katakana.size();
			else
				return 0;
			while (at < tag.size() && tag[at] == ' ')
				at++;
			int number = 0;
			size_t digits = 0;
			while (at < tag.size() && IsDigit(tag[at]) && digits < 2)
			{
				number = number * 10 + (tag[at] - '0');
				at++;
				digits++;
			}
			return (digits > 0) ? number : 0;
		}

		// A sample or a shop's disc, by the words Redump and PCSX2's database use for them.
		inline bool IsDemoTag(std::string_view tag)
		{
			static const char* const words[] = {"Demo", "Taikenban", "Trial", "Sample", "Kiosk", "Preview", "Beta", "Proto",
				"\xE4\xBD\x93\xE9\xA8\x93\xE7\x89\x88" /* 体験版 */};
			for (const char* const word : words)
			{
				if (tag.find(word) != std::string_view::npos)
					return true;
			}
			return false;
		}

		// A plain letter for the accented ones of Western European languages and of romaji
		// (U+00C0..U+017F), and for the full-width forms; "" for a character that has none here.
		inline const char* FoldCodepoint(uint32_t cp)
		{
			if (cp >= 0xFF01 && cp <= 0xFF5E)
			{
				static char wide[2];
				wide[0] = static_cast<char>(cp - 0xFF01 + 0x21);
				wide[1] = 0;
				return wide;
			}
			if (cp == 0x2019 || cp == 0x2018)
				return "'";
			if (cp == 0x3000)
				return " ";
			if (cp < 0xC0 || cp > 0x17F)
				return "";
			static const char* const latin1[] = {
				"A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I", // C0
				"D", "N", "O", "O", "O", "O", "O", "", "O", "U", "U", "U", "U", "Y", "", "ss", // D0
				"a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i", // E0
				"d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "", "y", // F0
			};
			if (cp <= 0xFF)
				return latin1[cp - 0xC0];
			// Latin Extended-A comes in pairs (capital, small) of one base letter.
			static const char* const extended = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi"
												"??JjKk?LlLlLlLlLlNnNnNn???OoOoOo??RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZz?";
			const char letter = extended[cp - 0x100];
			if (cp == 0x152)
				return "OE";
			if (cp == 0x153)
				return "oe";
			if (letter == '?')
				return "";
			static char one[2];
			one[0] = letter;
			one[1] = 0;
			return one;
		}

		// UTF-8 to plain ASCII: accents folded, what has no plain letter left out.
		inline std::string FoldToAscii(std::string_view text)
		{
			std::string out;
			out.reserve(text.size());
			for (size_t i = 0; i < text.size();)
			{
				const unsigned char c = static_cast<unsigned char>(text[i]);
				if (c < 0x80)
				{
					out.push_back(static_cast<char>(c));
					i++;
					continue;
				}
				uint32_t cp = 0;
				size_t length = 1;
				if ((c & 0xE0) == 0xC0)
				{
					cp = c & 0x1F;
					length = 2;
				}
				else if ((c & 0xF0) == 0xE0)
				{
					cp = c & 0x0F;
					length = 3;
				}
				else if ((c & 0xF8) == 0xF0)
				{
					cp = c & 0x07;
					length = 4;
				}
				else
				{
					i++;
					continue;
				}
				if (i + length > text.size())
					break;
				for (size_t k = 1; k < length; k++)
					cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
				i += length;
				out += FoldCodepoint(cp);
			}
			return out;
		}

		// "ii" -> 2 ... "xxxix" -> 39; 0 when the word is not a roman numeral of i, v and x.
		inline int RomanNumeral(std::string_view word)
		{
			if (word.empty() || word.size() > 6)
				return 0;
			int total = 0;
			int previous = 0;
			for (size_t i = word.size(); i-- > 0;)
			{
				int value;
				switch (word[i])
				{
					case 'i':
						value = 1;
						break;
					case 'v':
						value = 5;
						break;
					case 'x':
						value = 10;
						break;
					default:
						return 0;
				}
				total += (value < previous) ? -value : value;
				previous = std::max(previous, value);
			}
			if (total < 1 || total > 39)
				return 0;
			// Only the one right spelling of each number ("iiii" and "vx" are not numerals).
			static const char* const ones[] = {"", "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix"};
			static const char* const tens[] = {"", "x", "xx", "xxx"};
			std::string spelled = tens[total / 10];
			spelled += ones[total % 10];
			return (spelled == word) ? total : 0;
		}

		// The words that go to the end of a Redump title after a comma ("Sims, The").
		inline bool IsArticle(std::string_view word)
		{
			static const char* const articles[] = {"the", "a", "an", "le", "la", "les", "l", "der", "die", "das", "el", "los", "las", "il", "lo",
				"gli", "i", "de", "het", "een", "un", "une", "uno", "una", "os", "as", "o"};
			for (const char* const article : articles)
			{
				if (word == article)
					return true;
			}
			return false;
		}
	} // namespace detail

	/// A title in the one form titles are compared in: small letters and digits only, no spaces.
	/// `ends` gets where each part of the title ends in it (the parts are what " - " or ": "
	/// separate: "Title - Subtitle" has two), the last being the key's length.
	inline std::string NormaliseTitle(std::string_view title, std::vector<uint8_t>* ends = nullptr)
	{
		const std::string ascii = detail::FoldToAscii(title);
		std::string key;
		key.reserve(ascii.size());
		if (ends)
			ends->clear();

		struct Word
		{
			std::string text;
			bool comma_before = false; // ", The"
		};
		std::vector<Word> words;
		const auto flush_part = [&]() {
			if (words.empty())
				return;
			// "Sims, The" is "The Sims"; and "The" at the front says nothing ("The Sims" is "Sims"
			// in some lists), so it goes altogether. Other articles go to the front.
			if (words.size() >= 2 && words.back().comma_before && detail::IsArticle(words.back().text))
			{
				Word article = std::move(words.back());
				words.pop_back();
				words.insert(words.begin(), std::move(article));
			}
			if (words.size() >= 2 && words.front().text == "the")
				words.erase(words.begin());
			const size_t before = key.size();
			for (const Word& word : words)
			{
				const int number = detail::RomanNumeral(word.text);
				if (number > 0)
					key += std::to_string(number);
				else
					key += word.text;
			}
			words.clear();
			if (ends && key.size() > before && key.size() < 256)
				ends->push_back(static_cast<uint8_t>(key.size()));
		};

		std::string word;
		bool comma = false;
		bool word_comma = false;
		const auto flush_word = [&]() {
			if (word.empty())
				return;
			words.push_back({std::move(word), word_comma});
			word.clear();
		};
		for (size_t i = 0; i < ascii.size(); i++)
		{
			const char c = ascii[i];
			if (detail::IsAlnum(c))
			{
				if (word.empty())
				{
					word_comma = comma;
					comma = false;
				}
				word.push_back(detail::Lower(c));
				continue;
			}
			if (c == '\'')
				continue; // "Hawk's" is one word
			flush_word();
			if (c == ',')
			{
				comma = true;
				continue;
			}
			const bool spaced = (i > 0 && ascii[i - 1] == ' ' && i + 1 < ascii.size() && ascii[i + 1] == ' ');
			if (c == '&' || (c == '_' && spaced) || (c == '+' && spaced))
			{
				// "&" is "_" in libretro's names, and "and" in some lists.
				words.push_back({"and", false});
				comma = false;
			}
			else if ((c == '-' && spaced) || (c == ':' && i + 1 < ascii.size() && ascii[i + 1] == ' ') || c == '~')
			{
				flush_part();
				comma = false;
			}
			else if (c != ' ')
			{
				comma = false;
			}
		}
		flush_word();
		flush_part();
		if (ends && (ends->empty() || ends->back() != key.size()))
		{
			// (a key longer than 255: one part)
			ends->clear();
			ends->push_back(static_cast<uint8_t>(std::min<size_t>(key.size(), 255)));
		}
		return key;
	}

	/// What a name says besides the title.
	struct NameInfo
	{
		std::string title; // as written, without the tags
		uint32_t regions = 0; // RegionBit; 0 when the name has no region tag
		uint32_t languages = 0; // a hash of the "(En,Fr)" tag, 0 without one
		uint32_t extra = 0; // a hash of the other tags ("(v2.00)", "(Rev 1)", "(Genteiban)"), 0 without any
		uint8_t disc = 0; // "(Disc 2)": 2
		uint8_t extra_count = 0;
		bool demo = false;
		bool english = false; // the language tag has "En"
		bool tagged = false; // the name has "(...)" or "[...]" tags at all
	};

	/// Takes a Redump-style name ("Title (USA) (En,Fr) (Disc 1)") or a title of PCSX2's database
	/// ("Title [Disc 1 of 2] [Demo]") apart. `language` is the game's own, to be found in a
	/// language tag ("Fr"); `has_language` then says whether the tag has it.
	inline NameInfo ParseName(std::string_view name, std::string_view language = {}, bool* has_language = nullptr)
	{
		NameInfo info;
		if (has_language)
			*has_language = false;

		// The title ends where the first tag that is not part of it starts: for a Redump name the
		// region tag (a few titles have brackets of their own before it), else the first tag.
		size_t title_end = std::string_view::npos;
		size_t first_tag = std::string_view::npos;
		for (size_t i = 0; i < name.size();)
		{
			const char open = name[i];
			if ((open != '(' && open != '[') || (i > 0 && name[i - 1] != ' '))
			{
				i++;
				continue;
			}
			const size_t close = name.find(open == '(' ? ')' : ']', i + 1);
			if (close == std::string_view::npos)
				break;
			const std::string_view tag = name.substr(i + 1, close - i - 1);
			if (first_tag == std::string_view::npos)
				first_tag = i;
			info.tagged = true;

			bool english = false;
			const uint32_t regions = (open == '(') ? detail::RegionTag(tag) : 0;
			const uint32_t languages = (open == '(' && info.regions != 0) ? detail::LanguageTag(tag, &english) : 0;
			const int disc = detail::DiscNumber(tag);
			if (regions != 0 && info.regions == 0)
			{
				info.regions = regions;
				if (title_end == std::string_view::npos)
					title_end = i;
			}
			else if (languages != 0)
			{
				info.languages = languages;
				info.english = english;
				if (has_language && !language.empty())
					*has_language = detail::TagHasLanguage(tag, language);
			}
			else if (disc > 0)
			{
				info.disc = static_cast<uint8_t>(std::min(disc, 99));
				if (open == '[' && title_end == std::string_view::npos)
					title_end = i;
			}
			else if (detail::IsDemoTag(tag))
			{
				info.demo = true;
				if (open == '[' && title_end == std::string_view::npos)
					title_end = i;
			}
			else if (info.regions != 0 || open == '[')
			{
				// An edition, a version: after the region in a Redump name, in [] in the database.
				for (const char c : tag)
					info.extra = (info.extra ^ static_cast<unsigned char>(c)) * 16777619u + 1;
				info.extra |= 1;
				info.extra_count++;
				if (open == '[' && title_end == std::string_view::npos)
					title_end = i;
			}
			i = close + 1;
		}
		if (title_end == std::string_view::npos)
			title_end = (info.regions == 0 && first_tag != std::string_view::npos && name[first_tag] == '[') ? first_tag : name.size();
		info.title = std::string(detail::Trim(name.substr(0, title_end)));
		return info;
	}

	/// The game's region as PCSX2's database writes it ("NTSC-U", "PAL-F", "PAL-M5", "NTSC-J"):
	/// where its cover is from first (`home`), what else is the same market (`near`), and its
	/// language's code in Redump's tags.
	struct GameRegion
	{
		uint32_t home = 0;
		uint32_t near = 0;
		const char* language = "En";
	};

	inline GameRegion RegionFromDatabase(std::string_view region)
	{
		GameRegion out;
		if (region.starts_with("NTSC-U"))
		{
			out.home = R_USA;
			out.near = R_AMERICAS | R_WORLD;
		}
		else if (region.starts_with("NTSC-J"))
		{
			out.home = R_JAPAN;
			out.near = R_ASIA | R_KOREA | R_WORLD;
			out.language = "Ja";
		}
		else if (region.starts_with("NTSC-K"))
		{
			out.home = R_KOREA;
			out.near = R_ASIA | R_JAPAN | R_WORLD;
			out.language = "Ko";
		}
		else if (region.starts_with("NTSC-C") || region.starts_with("NTSC-HK") || region.starts_with("NTSC-T") ||
				 region.starts_with("NTSC-B"))
		{
			out.home = R_ASIA;
			out.near = R_JAPAN | R_KOREA | R_WORLD;
			out.language = "Zh";
		}
		else if (region.starts_with("PAL"))
		{
			// One country's edition has its own cover when libretro has it; "(Europe)" otherwise.
			static const struct
			{
				const char* name;
				uint32_t bit;
				const char* language;
			} countries[] = {
				{"PAL-AU", R_AUSTRALIA, "En"},
				{"PAL-AF", 0, "En"},
				{"PAL-A", R_AUSTRALIA, "En"},
				{"PAL-BE", R_OTHER_EUROPE, "Fr"},
				{"PAL-FI", R_NORDIC, "Fi"},
				{"PAL-F", R_FRANCE, "Fr"},
				{"PAL-GR", R_OTHER_EUROPE, "El"},
				{"PAL-G", R_GERMANY, "De"},
				{"PAL-IN", R_INDIA, "En"},
				{"PAL-I", R_ITALY, "It"},
				{"PAL-NL", R_NETHERLANDS, "Nl"},
				{"PAL-NO", R_NORDIC, "No"},
				{"PAL-PL", R_POLAND, "Pl"},
				{"PAL-P", R_PORTUGAL, "Pt"},
				{"PAL-R", R_RUSSIA, "Ru"},
				{"PAL-SC", R_NORDIC, "Sv"},
				{"PAL-SWI", R_OTHER_EUROPE, "De"},
				{"PAL-SW", R_NORDIC, "Sv"},
				{"PAL-S", R_SPAIN, "Es"},
				{"PAL-UK", R_UK, "En"},
			};
			out.home = R_EUROPE;
			out.near = R_ANY_EUROPE | R_WORLD;
			for (const auto& country : countries)
			{
				if (region.starts_with(country.name))
				{
					// (both: the country's own picture is preferred by the score below)
					out.home = R_EUROPE | country.bit;
					out.language = country.language;
					break;
				}
			}
		}
		return out;
	}

	/// What is known of a game. Empty parts are left out of the search.
	struct Game
	{
		std::string_view file_stem; // the file's name without folder and extension
		std::string_view title; // PCSX2's database
		std::string_view title_en; // its English or romaji title, for a Japanese one
		std::string_view region; // "NTSC-U", "PAL-M5", ... ("" when unknown)
	};

	enum class MatchKind : uint8_t
	{
		None,
		FileName, // the file is named as the picture
		Title, // the same title in the one form
		Subtitle, // the same but for a subtitle one side leaves out
		Spelling, // the same but for a letter or two
	};

	struct Match
	{
		int index = -1; // into the list of names; -1: none
		MatchKind kind = MatchKind::None;
		bool own_region = false; // the picture is of the game's own region
	};

	inline const char* MatchKindName(MatchKind kind)
	{
		static const char* const names[] = {"none", "file name", "title", "subtitle", "spelling"};
		return names[static_cast<size_t>(kind)];
	}

	/// The list of box-art names (without ".png"), ready for looking games up: built once.
	class Index
	{
	public:
		/// Takes the names over. Empty names are dropped.
		void Build(std::vector<std::string> names)
		{
			m_names = std::move(names);
			std::erase_if(m_names, [](const std::string& name) { return name.empty(); });
			m_entries.clear();
			m_entries.reserve(m_names.size());
			for (size_t i = 0; i < m_names.size(); i++)
			{
				Entry entry;
				const NameInfo info = ParseName(m_names[i]);
				entry.key = NormaliseTitle(info.title, &entry.ends);
				entry.name = static_cast<uint32_t>(i);
				entry.regions = info.regions;
				entry.languages = info.languages;
				entry.extra = info.extra;
				entry.disc = info.disc;
				entry.extra_count = info.extra_count;
				entry.demo = info.demo;
				entry.english = info.english;
				if (!entry.key.empty())
					m_entries.push_back(std::move(entry));
			}
			std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });

			m_by_name.resize(m_names.size());
			for (size_t i = 0; i < m_names.size(); i++)
				m_by_name[i] = static_cast<uint32_t>(i);
			std::sort(m_by_name.begin(), m_by_name.end(),
				[this](uint32_t a, uint32_t b) { return detail::LessNoCase(m_names[a], m_names[b]); });
		}

		size_t Size() const { return m_names.size(); }
		const std::string& Name(int index) const { return m_names[static_cast<size_t>(index)]; }

		/// The picture for a game; index -1 when none is clearly its own.
		Match Find(const Game& game) const
		{
			Match match;
			if (m_names.empty())
				return match;
			const GameRegion region = RegionFromDatabase(game.region);

			// 1. The file is named as a picture is (libretro writes "_" for what a file name
			//    cannot have on every system; a file may have kept some of those).
			if (!game.file_stem.empty())
			{
				std::string stem(game.file_stem);
				for (char& c : stem)
				{
					if (std::strchr("&*/:`<>?\\|\"", c))
						c = '_';
				}
				const auto it = std::lower_bound(m_by_name.begin(), m_by_name.end(), std::string_view(stem),
					[this](uint32_t index, std::string_view name) { return detail::LessNoCase(m_names[index], name); });
				if (it != m_by_name.end() && detail::EqualNoCase(m_names[*it], stem))
				{
					match.index = static_cast<int>(*it);
					match.kind = MatchKind::FileName;
					match.own_region = true;
					return match;
				}
			}

			// What the game is called: by its file when that is named the Redump way (the name
			// then also says the region, the disc and the version, better than a serial does), and
			// by the database.
			Query queries[3];
			size_t count = 0;
			NameInfo wanted; // the region, disc, languages the pictures are scored against
			wanted.regions = 0;
			if (!game.file_stem.empty())
			{
				const NameInfo info = ParseName(game.file_stem);
				if (info.regions != 0)
				{
					wanted = info;
					queries[count].key = NormaliseTitle(info.title, &queries[count].ends);
					count++;
				}
			}
			for (const std::string_view title : {game.title, game.title_en})
			{
				if (title.empty())
					continue;
				const NameInfo info = ParseName(title);
				if (wanted.disc == 0)
					wanted.disc = info.disc;
				wanted.demo = wanted.demo || info.demo;
				Query query;
				query.key = NormaliseTitle(info.title, &query.ends);
				if (query.key.empty() || (count > 0 && queries[count - 1].key == query.key))
					continue;
				queries[count++] = std::move(query);
			}
			if (wanted.regions == 0 && !game.file_stem.empty() && count < 3)
			{
				// A file named some other way ("my game.iso") can still be the title.
				Query query;
				query.key = NormaliseTitle(ParseName(game.file_stem).title, &query.ends);
				if (!query.key.empty())
					queries[count++] = std::move(query);
			}

			// 2. The same title.
			for (size_t q = 0; q < count; q++)
			{
				const auto [first, last] = Range(queries[q].key);
				const Entry* best = nullptr;
				int best_score = 0;
				for (const Entry* entry = first; entry != last; entry++)
				{
					const int score = Score(*entry, wanted, region);
					if (!best || score > best_score)
					{
						best = entry;
						best_score = score;
					}
				}
				if (best)
				{
					match.index = static_cast<int>(best->name);
					match.kind = MatchKind::Title;
					match.own_region = IsHome(*best, wanted, region);
					return match;
				}
			}

			// 3. and 4. only among the pictures of the game's own region, and only when one title
			// alone fits: these are guesses, and a guess across regions or between two candidates
			// is how a game gets another game's cover.
			if (wanted.regions == 0 && region.home == 0)
				return match;
			for (size_t q = 0; q < count; q++)
			{
				if (const Entry* const entry = FindSubtitle(queries[q], wanted, region))
				{
					match.index = static_cast<int>(entry->name);
					match.kind = MatchKind::Subtitle;
					match.own_region = true;
					return match;
				}
			}
			for (size_t q = 0; q < count; q++)
			{
				if (const Entry* const entry = FindSpelling(queries[q], wanted, region))
				{
					match.index = static_cast<int>(entry->name);
					match.kind = MatchKind::Spelling;
					match.own_region = true;
					return match;
				}
			}
			return match;
		}

	private:
		struct Entry
		{
			std::string key;
			std::vector<uint8_t> ends;
			uint32_t name = 0;
			uint32_t regions = 0;
			uint32_t languages = 0;
			uint32_t extra = 0;
			uint8_t disc = 0;
			uint8_t extra_count = 0;
			bool demo = false;
			bool english = false;
		};

		struct Query
		{
			std::string key;
			std::vector<uint8_t> ends;
		};

		std::pair<const Entry*, const Entry*> Range(std::string_view key) const
		{
			const auto first = std::lower_bound(m_entries.begin(), m_entries.end(), key,
				[](const Entry& entry, std::string_view value) { return std::string_view(entry.key) < value; });
			auto last = first;
			while (last != m_entries.end() && last->key == key)
				++last;
			const Entry* const base = m_entries.data();
			return {base + (first - m_entries.begin()), base + (last - m_entries.begin())};
		}

		static bool IsHome(const Entry& entry, const NameInfo& wanted, const GameRegion& region)
		{
			const uint32_t home = (wanted.regions != 0) ? wanted.regions : region.home;
			return (entry.regions & home) != 0;
		}

		// How well a picture of the right title fits the game: region first, then demo or not,
		// then the disc, then the plainest name.
		static int Score(const Entry& entry, const NameInfo& wanted, const GameRegion& region)
		{
			int score = 0;
			if (wanted.regions != 0)
			{
				// The file's own name says it.
				if (entry.regions == wanted.regions)
					score += 1200;
				else if (entry.regions & wanted.regions)
					score += 1000;
				if (wanted.languages != 0 && entry.languages == wanted.languages)
					score += 60;
				if (entry.extra == wanted.extra)
					score += 30;
			}
			if (region.home != 0)
			{
				// A country's own edition before "(Europe)", that before the rest of the market.
				const uint32_t country = region.home & ~static_cast<uint32_t>(R_EUROPE);
				if (country != 0 && (entry.regions & country))
					score += 900;
				else if (entry.regions & region.home)
					score += 800;
				else if (entry.regions & region.near)
					score += 500;
				else if (entry.regions & (R_USA | R_EUROPE | R_UK | R_AUSTRALIA))
					score += 300; // a cover in English at least
				else
					score += 100;
			}
			else if (wanted.regions == 0)
			{
				// Nothing known: the most common ones first.
				if (entry.regions & R_USA)
					score += 800;
				else if (entry.regions & R_EUROPE)
					score += 700;
				else if (entry.regions & R_JAPAN)
					score += 600;
			}

			if (entry.demo == wanted.demo)
				score += 200;

			if (wanted.disc != 0)
			{
				if (entry.disc == wanted.disc)
					score += 80;
				else if (entry.disc == 0)
					score += 60;
				else if (entry.disc == 1)
					score += 20;
			}
			else
			{
				if (entry.disc == 0)
					score += 80;
				else if (entry.disc == 1)
					score += 60;
			}

			if (entry.languages == 0 || entry.english)
				score += 5;
			// An edition or a later version only when nothing plainer is there.
			score -= 10 * std::min<int>(entry.extra_count, 3);
			return score;
		}

		static bool Digits(std::string_view a, std::string_view b)
		{
			// The same digits in the same order: "2004" is never "2005", "2" never "3".
			size_t i = 0, j = 0;
			for (;;)
			{
				while (i < a.size() && !detail::IsDigit(a[i]))
					i++;
				while (j < b.size() && !detail::IsDigit(b[j]))
					j++;
				if (i == a.size() || j == b.size())
					return i == a.size() && j == b.size();
				if (a[i] != b[j])
					return false;
				i++;
				j++;
			}
		}

		// Of the candidates of a guess: the best picture when they are all one title, else none.
		const Entry* OneTitle(const std::vector<const Entry*>& candidates, const NameInfo& wanted, const GameRegion& region) const
		{
			const Entry* best = nullptr;
			int best_score = 0;
			for (const Entry* const entry : candidates)
			{
				if (best && entry->key != candidates.front()->key)
					return nullptr;
				const int score = Score(*entry, wanted, region);
				if (!best || score > best_score)
				{
					best = entry;
					best_score = score;
				}
			}
			return best;
		}

		// 3. "Title" for "Title - Subtitle" or the other way round.
		const Entry* FindSubtitle(const Query& query, const NameInfo& wanted, const GameRegion& region) const
		{
			// What both have must be enough to be a title by itself.
			static constexpr size_t MIN_SHARED = 6;
			std::vector<const Entry*> candidates;

			// The picture has more: every key that starts with the query's and has a part ending there.
			if (query.key.size() >= MIN_SHARED)
			{
				auto it = std::lower_bound(m_entries.begin(), m_entries.end(), std::string_view(query.key),
					[](const Entry& entry, std::string_view value) { return std::string_view(entry.key) < value; });
				for (; it != m_entries.end() && it->key.starts_with(query.key); ++it)
				{
					if (it->key.size() == query.key.size() || !IsHome(*it, wanted, region))
						continue;
					if (std::find(it->ends.begin(), it->ends.end(), static_cast<uint8_t>(query.key.size())) != it->ends.end())
						candidates.push_back(&*it);
				}
			}
			// The game has more: the query cut at each of its parts.
			for (size_t i = 0; i + 1 < query.ends.size(); i++)
			{
				const size_t length = query.ends[i];
				if (length < MIN_SHARED)
					continue;
				const auto [first, last] = Range(std::string_view(query.key).substr(0, length));
				for (const Entry* entry = first; entry != last; entry++)
				{
					if (IsHome(*entry, wanted, region))
						candidates.push_back(entry);
				}
			}
			if (candidates.empty())
				return nullptr;
			return OneTitle(candidates, wanted, region);
		}

		// Whether two keys are the same but for `limit` letters changed, missing or added.
		static bool Near(std::string_view a, std::string_view b, int limit)
		{
			if (a.size() > b.size())
				std::swap(a, b);
			if (b.size() - a.size() > static_cast<size_t>(limit) || b.size() > 250)
				return false;
			// Two rows of the classic table are enough.
			uint8_t rows[2][256];
			uint8_t* previous = rows[0];
			uint8_t* current = rows[1];
			for (size_t j = 0; j <= b.size(); j++)
				previous[j] = static_cast<uint8_t>(j);
			for (size_t i = 1; i <= a.size(); i++)
			{
				current[0] = static_cast<uint8_t>(i);
				int row_min = current[0];
				for (size_t j = 1; j <= b.size(); j++)
				{
					const int change = previous[j - 1] + (a[i - 1] != b[j - 1] ? 1 : 0);
					const int value = std::min({change, previous[j] + 1, current[j - 1] + 1});
					current[j] = static_cast<uint8_t>(value);
					row_min = std::min(row_min, value);
				}
				if (row_min > limit)
					return false;
				std::swap(previous, current);
			}
			return previous[b.size()] <= limit;
		}

		// 4. A letter or two.
		const Entry* FindSpelling(const Query& query, const NameInfo& wanted, const GameRegion& region) const
		{
			const size_t length = query.key.size();
			if (length < 10)
				return nullptr;
			const int limit = (length >= 18) ? 2 : 1;
			std::vector<const Entry*> candidates;
			for (const Entry& entry : m_entries)
			{
				const size_t other = entry.key.size();
				if (other + limit < length || other > length + limit || !IsHome(entry, wanted, region))
					continue;
				// The first letters are rarely where a title is spelled another way; it is where
				// two titles differ.
				if (entry.key[0] != query.key[0] || !Digits(entry.key, query.key))
					continue;
				if (Near(entry.key, query.key, limit))
					candidates.push_back(&entry);
			}
			if (candidates.empty())
				return nullptr;
			return OneTitle(candidates, wanted, region);
		}

		std::vector<std::string> m_names;
		std::vector<Entry> m_entries; // by key
		std::vector<uint32_t> m_by_name; // names in order, case left aside
	};

	//////////////////////////////////////////////////////////////////////////
	// Files
	//////////////////////////////////////////////////////////////////////////

	/// The list file's text (one name a line) to names; lines starting with '#' are notes.
	inline std::vector<std::string> ParseNameList(std::string_view text)
	{
		std::vector<std::string> names;
		names.reserve(text.size() / 40 + 1);
		while (!text.empty())
		{
			const size_t end = text.find('\n');
			const std::string_view line = detail::Trim(text.substr(0, end));
			if (!line.empty() && line.front() != '#')
				names.emplace_back(line);
			text = (end == std::string_view::npos) ? std::string_view() : text.substr(end + 1);
		}
		return names;
	}

	/// Whether these bytes start as a PNG file does.
	inline bool IsPNG(const void* data, size_t size)
	{
		static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
		return size > 64 && std::memcmp(data, signature, sizeof(signature)) == 0;
	}

	/// libretro keeps one picture for several names as a link, and a link's bytes are the name of
	/// the file it stands for ("Other Name (Europe).png"). That name when the bytes are one, else "".
	inline std::string LinkTarget(const void* data, size_t size)
	{
		const std::string_view text = detail::Trim(std::string_view(static_cast<const char*>(data), size));
		if (text.size() < 5 || text.size() > 300 || !text.ends_with(".png"))
			return {};
		for (const char c : text)
		{
			// A name in the same folder and nothing else.
			if (static_cast<unsigned char>(c) < 0x20 || c == '/' || c == '\\')
				return {};
		}
		return std::string(text);
	}

	/// How a game is written in the file of games that have no picture: one line.
	inline std::string NoMatchLine(std::string_view serial, std::string_view file_stem)
	{
		std::string line(serial);
		line += '\t';
		line += file_stem;
		return line;
	}
} // namespace PS5Covers
