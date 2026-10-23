// Crux2 - the game list by letter: which group a title is in, where a jump to the next or
// the previous group lands, and the order titles are sorted in (so that a letter's games are
// together). Plain functions, so the PC-side test (ps5/tests/letter_jump_test.cpp) builds them alone.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

/// The group a title is in: its first character as a capital letter, or '#' for a digit or
/// anything else (a title that is empty, or starts with a space or a character outside A-Z). The
/// first character as it is, with nothing skipped: that is what the titles are sorted by, and a
/// group has to be one run of the sorted list.
inline char PS5LetterGroup(std::string_view text)
{
	if (text.empty())
		return '#';

	const char c = text.front();
	if (c >= 'a' && c <= 'z')
		return static_cast<char>(c - 'a' + 'A');
	return (c >= 'A' && c <= 'Z') ? c : '#';
}

/// Where the cursor goes from entry `current` of `count` (current == count: it is on none).
/// Forward (direction > 0): the first entry after the current entry's group, and from the last
/// group the list's first entry. Back: the first entry of the group before the current entry's,
/// and from the first group the first entry of the last. group_of(index) names an entry's group;
/// entries of one group are expected next to each other, and where they are not, each run of
/// them counts as a group of its own.
template <typename GroupOf>
inline size_t PS5LetterJumpTarget(size_t count, size_t current, int direction, GroupOf&& group_of)
{
	if (count == 0 || current >= count)
		return 0;

	const char group = group_of(current);
	if (direction > 0)
	{
		size_t target = current + 1;
		while (target < count && group_of(target) == group)
			target++;
		return (target == count) ? 0 : target;
	}

	size_t start = current;
	while (start > 0 && group_of(start - 1) == group)
		start--;

	size_t target = (start == 0) ? (count - 1) : (start - 1);
	const char previous = group_of(target);
	while (target > 0 && group_of(target - 1) == previous)
		target--;
	return target;
}

/// The order of two titles: without regard to case, as the desktop interface's collation orders
/// them, so that a letter's games are together ("ico" beside "Ico"). Titles equal but for case:
/// capitals first, so the order is always the same. Negative, zero or positive, as strcmp().
inline int PS5CompareTitles(std::string_view lhs, std::string_view rhs)
{
	const size_t length = (lhs.size() < rhs.size()) ? lhs.size() : rhs.size();
	for (size_t i = 0; i < length; i++)
	{
		const unsigned char a = static_cast<unsigned char>(lhs[i]);
		const unsigned char b = static_cast<unsigned char>(rhs[i]);
		const unsigned char la = (a >= 'A' && a <= 'Z') ? static_cast<unsigned char>(a - 'A' + 'a') : a;
		const unsigned char lb = (b >= 'A' && b <= 'Z') ? static_cast<unsigned char>(b - 'A' + 'a') : b;
		if (la != lb)
			return (la < lb) ? -1 : 1;
	}
	if (lhs.size() != rhs.size())
		return (lhs.size() < rhs.size()) ? -1 : 1;

	const int result = (length > 0) ? std::memcmp(lhs.data(), rhs.data(), length) : 0;
	return (result < 0) ? -1 : ((result > 0) ? 1 : 0);
}
