// Crux2 - the saves on the memory cards (ps5_cards.h says what and how).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_cards.h"

#include "ps5_backup.h"
#include "ps5_mc.h"

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace PS5Cards
{
namespace
{
// Larger than any card PCSX2 makes (64 MB with its error-correction bytes), and than any save.
constexpr uint64_t LARGEST_CARD = 80ull << 20, LARGEST_SAVE_FILE = 70ull << 20;

std::string FileName(const std::string& path)
{
	const size_t slash = path.rfind('/');
	return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

std::string Title(const std::string& path)
{
	std::string name = FileName(path);
	const size_t dot = name.rfind('.');
	if (dot != std::string::npos && dot > 0)
		name.erase(dot);
	return name;
}

std::string Join(const std::string& folder, const std::string& name)
{
	return (!folder.empty() && folder.back() == '/') ? folder + name : folder + "/" + name;
}

/// The moment, as a card keeps it: by convention the clock of Japan.
PS5Mc::Stamp Now()
{
	const time_t japan = time(nullptr) + 9 * 3600;
	struct tm t;
	if (gmtime_r(&japan, &t) == nullptr)
		return PS5Mc::Stamp::Of(2026, 1, 1, 0, 0, 0);
	return PS5Mc::Stamp::Of(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

bool ReadBytes(const std::string& path, PS5Mc::Bytes* bytes)
{
	std::string data;
	if (!PS5Backup::ReadWholeFile(path, &data))
		return false;
	bytes->assign(data.begin(), data.end());
	return true;
}

bool WriteBytes(const std::string& path, const PS5Mc::Bytes& bytes)
{
	return PS5Backup::WriteWholeFile(path, std::string(bytes.begin(), bytes.end()));
}

// Held while a card's file is replaced (ps5_cards.h, SetWriteGuard).
std::mutex s_write_mutex;
std::function<bool()> s_write_allowed;

/// Puts a card's new bytes in the place of its file, keeping the bytes that were there (`before`)
/// in the undo folder first.
bool ReplaceCard(const std::string& path, const PS5Mc::Bytes& now, const PS5Mc::Bytes& before, const std::string& undo, std::string* why)
{
	const std::lock_guard lock(s_write_mutex);
	if (s_write_allowed && !s_write_allowed())
	{
		*why = "A game was started meanwhile: the card is as it was.";
		return false;
	}
	if (!undo.empty())
	{
		const size_t slash = path.rfind('/');
		const std::string kept = undo + "/" + ((slash == std::string::npos) ? path : path.substr(slash + 1));
		if (!PS5Backup::MakeAllFolders(undo) || !WriteBytes(kept, before))
		{
			*why = (errno == ENOSPC) ? "There is no room to keep the card as it was: nothing was changed." :
			                           "The card as it was could not be kept: nothing was changed.";
			return false;
		}
	}
	if (!WriteBytes(path, now))
	{
		*why = (errno == ENOSPC) ? "There is no room to write the card: it is as it was." : "The card could not be written: it is as it was.";
		return false;
	}
	return true;
}

/// A card opened for a change: its bytes as they are on the drive, and the card made of them
/// (made ready first when nobody ever formatted it).
struct Opened
{
	PS5Mc::Bytes before;
	PS5Mc::Card card;
	bool formatted = false; // it was made ready here
};

bool Open(const std::string& path, Opened* opened, std::string* why)
{
	if (!ReadBytes(path, &opened->before))
	{
		*why = "The card's file could not be read.";
		return false;
	}
	if (opened->before.size() > LARGEST_CARD)
	{
		*why = "The file is larger than any memory card.";
		return false;
	}
	PS5Mc::Bytes image = opened->before;
	if (PS5Mc::Card::Unformatted(image))
	{
		if (!PS5Mc::Card::Format(&image, Now()))
		{
			*why = "The card was never formatted, and its file is of no size a card can be made of here.";
			return false;
		}
		opened->formatted = true;
	}
	return opened->card.Open(std::move(image), why);
}

std::string UndoPath(const std::string& card, const std::string& undo)
{
	return undo.empty() ? std::string() : Join(undo, FileName(card));
}

/// Puts a changed card in the place of the one on the drive, keeping the one that was.
bool Commit(const std::string& path, const Opened& opened, const std::string& undo, std::string* why)
{
	return ReplaceCard(path, opened.card.Image(), opened.before, undo, why);
}

std::string Shown(const PS5Mc::Save& save)
{
	for (const PS5Mc::File& file : save.files)
	{
		if (file.name == "icon.sys")
		{
			const std::string title = PS5Mc::TitleOf(file.data);
			if (!title.empty())
				return title;
		}
	}
	return save.name;
}

/// A name a drive of any kind takes: what exFAT and Windows shares refuse becomes an underscore.
std::string SafeName(const std::string& name)
{
	std::string out;
	for (const char c : name)
	{
		const unsigned char u = static_cast<unsigned char>(c);
		out.push_back((u < 0x20 || u >= 0x7f || std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos) ? '_' : c);
	}
	while (!out.empty() && (out.back() == '.' || out.back() == ' '))
		out.back() = '_';
	return out.empty() ? std::string("save") : out;
}

Result Fail(std::string message)
{
	return Result{false, std::move(message)};
}
} // namespace

void SetWriteGuard(std::function<bool()> allowed)
{
	const std::lock_guard lock(s_write_mutex);
	s_write_allowed = std::move(allowed);
}

void WaitForWrites()
{
	const std::lock_guard lock(s_write_mutex);
}

std::string Size(uint64_t bytes)
{
	char out[32];
	if (bytes >= (1ull << 20))
		std::snprintf(out, sizeof(out), "%.1f MB", static_cast<double>(bytes) / (1ull << 20));
	else
		std::snprintf(out, sizeof(out), "%.0f KB", static_cast<double>(bytes) / 1024.0);
	return out;
}

CardInfo Look(const std::string& path)
{
	CardInfo info;
	PS5Mc::Bytes image;
	if (!ReadBytes(path, &image))
	{
		info.why = "The card's file could not be read.";
		return info;
	}
	if (image.size() > LARGEST_CARD)
	{
		info.why = "The file is larger than any memory card.";
		return info;
	}
	if (PS5Mc::Card::Unformatted(image))
	{
		if (!PS5Mc::Card::Formattable(image.size()))
		{
			info.why = "The card was never formatted, and its file is of no size a card can be made of here.";
			return info;
		}
		// (What it will hold once it is made ready: asked of a card made of the same bytes.)
		PS5Mc::Card card;
		std::string why;
		if (PS5Mc::Card::Format(&image, Now()) && card.Open(std::move(image), &why))
		{
			info.free_bytes = card.FreeBytes();
			info.total_bytes = card.TotalBytes();
		}
		info.state = CardInfo::New;
		return info;
	}
	PS5Mc::Card card;
	if (!card.Open(std::move(image), &info.why))
		return info;
	info.state = CardInfo::Listed;
	info.free_bytes = card.FreeBytes();
	info.total_bytes = card.TotalBytes();
	for (const PS5Mc::Card::Listed& one : card.List())
	{
		SaveInfo save;
		save.name = one.name;
		save.title = one.title;
		save.when = one.modified.Text();
		save.bytes = one.bytes;
		save.files = one.files;
		save.ps1 = one.ps1;
		info.saves.push_back(std::move(save));
	}
	return info;
}

bool IsSaveFile(const std::string& name)
{
	const size_t dot = name.rfind('.');
	if (dot == std::string::npos || dot == 0 || name[0] == '.')
		return false;
	std::string extension = name.substr(dot + 1);
	for (char& c : extension)
		c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	return extension == "psu" || extension == "max" || extension == "cbs" || extension == "sps" || extension == "xps" || extension == "psv";
}

std::vector<SaveFile> SaveFiles(const std::string& folder)
{
	std::vector<SaveFile> out;
	std::vector<std::pair<std::string, uint64_t>> files;
	if (!PS5Backup::ListFiles(folder, &files))
		return out;
	for (const auto& [name, size] : files)
	{
		if (IsSaveFile(name) && size > 0 && size <= LARGEST_SAVE_FILE)
			out.push_back({Join(folder, name), name, size});
	}
	return out;
}

Result Import(const std::string& path, const std::string& file, const std::string& undo)
{
	PS5Mc::Bytes bytes;
	if (!ReadBytes(file, &bytes))
		return Fail("The save file could not be read.");
	if (bytes.size() > LARGEST_SAVE_FILE)
		return Fail("The file is larger than any save.");
	std::string why;
	PS5Mc::Save save;
	const PS5Mc::Stamp now = Now();
	if (!PS5Mc::ReadSaveFile(bytes, &save, now, &why))
		return Fail(why);
	Opened opened;
	if (!Open(path, &opened, &why))
		return Fail(why);
	if (!opened.card.Write(save, now, &why))
	{
		if (why == "There is not enough room on the card.")
			why = "There is not enough room on the card: the save needs " + Size(static_cast<uint64_t>(opened.card.ClustersFor(save)) * opened.card.ClusterSize()) +
			      ", and " + Size(opened.card.FreeBytes()) + " are free.";
		return Fail(why);
	}
	if (!Commit(path, opened, undo, &why))
		return Fail(why);
	return Result{true, "\"" + Shown(save) + "\" is on " + Title(path) + " now" + (opened.formatted ? " (the card was new: it was made ready first)" : "") + ". " +
	                        Size(opened.card.FreeBytes()) + " are free."};
}

Result Export(const std::string& path, const std::string& name, const std::string& folder)
{
	Opened opened;
	std::string why;
	if (!Open(path, &opened, &why))
		return Fail(why);
	PS5Mc::Save save;
	if (!opened.card.Read(name, &save, &why))
		return Fail(why);
	if (!PS5Backup::MakeAllFolders(folder))
		return Fail(PS5Backup::WhyNot(errno, folder));
	const std::string base = SafeName(save.name);
	std::string out;
	for (int number = 1; number < 1000; number++)
	{
		out = Join(folder, (number == 1) ? base + ".psu" : base + " (" + std::to_string(number) + ").psu");
		if (!PS5Backup::PathExists(out))
			break;
		out.clear();
	}
	if (out.empty())
		return Fail("There are too many files of that name there already.");
	if (!WriteBytes(out, PS5Mc::WritePSU(save)))
		return Fail(PS5Backup::WhyNot(errno, folder));
	return Result{true, "\"" + Shown(save) + "\" was written as " + FileName(out) + "."};
}

Result Copy(const std::string& from, const std::string& name, const std::string& to, const std::string& undo)
{
	if (from == to)
		return Fail("That is the card the save is on.");
	Opened source, target;
	std::string why;
	if (!Open(from, &source, &why))
		return Fail(why);
	PS5Mc::Save save;
	if (!source.card.Read(name, &save, &why))
		return Fail(why);
	if (!Open(to, &target, &why))
		return Fail(Title(to) + ": " + why);
	if (!target.card.Write(save, Now(), &why))
	{
		if (why == "There is not enough room on the card.")
			why = "There is not enough room on " + Title(to) + ": the save needs " +
			      Size(static_cast<uint64_t>(target.card.ClustersFor(save)) * target.card.ClusterSize()) + ", and " + Size(target.card.FreeBytes()) + " are free.";
		else if (why == "A save of that name is on the card already.")
			why = "A save of that name is on " + Title(to) + " already: delete it there first to put this one in its place.";
		return Fail(why);
	}
	if (!Commit(to, target, undo, &why))
		return Fail(why);
	return Result{true, "\"" + Shown(save) + "\" is on " + Title(to) + " too now" + (target.formatted ? " (the card was new: it was made ready first)" : "") + "."};
}

Result Delete(const std::string& path, const std::string& name, const std::string& undo)
{
	Opened opened;
	std::string why;
	if (!Open(path, &opened, &why))
		return Fail(why);
	PS5Mc::Save save;
	const std::string shown = opened.card.Read(name, &save, &why) ? Shown(save) : name;
	if (!opened.card.Remove(name, Now(), &why))
		return Fail(why);
	if (!Commit(path, opened, undo, &why))
		return Fail(why);
	return Result{true, "\"" + shown + "\" was deleted from " + Title(path) + ". " + Size(opened.card.FreeBytes()) + " are free."};
}

// ------------------------------------------------------------------------------- whole cards

namespace
{
std::string Lower(std::string text)
{
	for (char& c : text)
		c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	return text;
}

/// A card without the error-correction bytes, given them: every page of 512 bytes followed by
/// the 12 bytes of its four 128-byte parts' correction and four of nothing, as PCSX2 keeps cards
/// (an erased page: 0xFF in all 16).
PS5Mc::Bytes WithEcc(const PS5Mc::Bytes& plain)
{
	const size_t pages = plain.size() / 512;
	PS5Mc::Bytes out(pages * 528, 0);
	for (size_t page = 0; page < pages; page++)
	{
		const uint8_t* const from = plain.data() + page * 512;
		uint8_t* const to = out.data() + page * 528;
		std::memcpy(to, from, 512);
		// An erased page (every byte 0xFF, as the card's spare blocks are) is erased whole.
		bool erased = true;
		for (size_t i = 0; i < 512 && erased; i++)
			erased = (from[i] == 0xff);
		if (erased)
		{
			std::memset(to + 512, 0xff, 16);
			continue;
		}
		for (int chunk = 0; chunk < 4; chunk++)
			PS5Mc::detail::Ecc(from + chunk * 128, to + 512 + chunk * 3);
	}
	return out;
}

bool PlainSize(size_t size)
{
	for (const size_t k : {1u, 2u, 4u, 8u})
	{
		if (size == k * PS5Mc::Card::SIZE_8MB_PLAIN)
			return true;
	}
	return false;
}

/// "Card 1.ps2" in a folder: the first of "<base>.ps2", "<base> (2).ps2"... that is not there.
std::string FreeName(const std::string& folder, const std::string& base, const char* ending)
{
	for (int number = 1; number < 1000; number++)
	{
		const std::string path = Join(folder, (number == 1) ? base + ending : base + " (" + std::to_string(number) + ")" + ending);
		if (!PS5Backup::PathExists(path))
			return path;
	}
	return std::string();
}
} // namespace

bool IsCardImageName(const std::string& name)
{
	const size_t dot = name.rfind('.');
	if (dot == std::string::npos || dot == 0 || name[0] == '.')
		return false;
	const std::string extension = Lower(name.substr(dot + 1));
	return extension == "ps2" || extension == "mcd" || extension == "mc2" || extension == "bin" || extension == "vmc" || extension == "vm2";
}

bool IsCardImageSize(uint64_t size)
{
	return PS5Mc::Card::Formattable(static_cast<size_t>(size));
}

Result ImportCard(const std::string& from, const std::string& to)
{
	if (PS5Backup::PathExists(to))
		return Fail("A card of that name is there already.");
	PS5Mc::Bytes image;
	if (!ReadBytes(from, &image))
		return Fail("The file could not be read.");
	if (!IsCardImageSize(image.size()))
		return Fail("The file is of no memory card's size (8, 16, 32 or 64 MB).");
	if (!PS5Mc::Card::Unformatted(image))
	{
		PS5Mc::Card check;
		std::string why;
		if (!check.Open(image, &why))
			return Fail("It does not read as a PlayStation 2 memory card: " + why);
	}
	const bool plain = PlainSize(image.size());
	if (plain)
		image = WithEcc(image);
	if (!WriteBytes(to, image))
		return Fail(PS5Backup::WhyNot(errno, to));
	PS5Mc::Card card;
	std::string why;
	const PS5Mc::Bytes written = image;
	if (PS5Mc::Card::Unformatted(written))
		return Result{true, "\"" + Title(to) + "\" was brought in (a card nobody formatted yet)."};
	card.Open(written, &why);
	return Result{true, "\"" + Title(to) + "\" was brought in: " + std::to_string(card.List().size()) + " saves, " + Size(card.FreeBytes()) + " free" +
	                        (plain ? " (given the error-correction bytes PCSX2 keeps)." : ".")};
}

Result ExportCard(const std::string& card, const std::string& folder)
{
	PS5Mc::Bytes image;
	if (!ReadBytes(card, &image))
		return Fail("The card's file could not be read.");
	if (!PS5Backup::MakeAllFolders(folder))
		return Fail(PS5Backup::WhyNot(errno, folder));
	const std::string out = FreeName(folder, SafeName(Title(card)), ".ps2");
	if (out.empty())
		return Fail("There are too many files of that name there already.");
	if (!WriteBytes(out, image))
		return Fail(PS5Backup::WhyNot(errno, folder));
	return Result{true, "\"" + Title(card) + "\" was written as " + FileName(out) + "."};
}

Result Create(const std::string& path, int megabytes)
{
	if (megabytes != 8 && megabytes != 16 && megabytes != 32 && megabytes != 64)
		return Fail("A card is of 8, 16, 32 or 64 MB.");
	if (PS5Backup::PathExists(path))
		return Fail("A card of that name is there already.");
	PS5Mc::Bytes image(static_cast<size_t>(megabytes / 8) * PS5Mc::Card::SIZE_8MB_ECC, 0);
	if (!PS5Mc::Card::Format(&image, Now()))
		return Fail("The card could not be made.");
	if (!WriteBytes(path, image))
		return Fail(PS5Backup::WhyNot(errno, path));
	return Result{true, "\"" + Title(path) + "\" was made: " + std::to_string(megabytes) + " MB, empty."};
}

Result Format(const std::string& card, const std::string& undo)
{
	PS5Mc::Bytes before;
	if (!ReadBytes(card, &before))
		return Fail("The card's file could not be read.");
	if (!IsCardImageSize(before.size()))
		return Fail("The card's file is of no size a card can be made of here.");
	PS5Mc::Bytes image(before.size(), 0);
	if (!PS5Mc::Card::Format(&image, Now()))
		return Fail("The card could not be formatted.");
	std::string why;
	if (!ReplaceCard(card, image, before, undo, &why))
		return Fail(why);
	PS5Mc::Card check;
	check.Open(image, &why);
	return Result{true, Title(card) + " is formatted: empty, " + Size(check.FreeBytes()) + " free."};
}

bool LooksLikeSaveFolder(const std::vector<std::string>& file_names)
{
	for (const std::string& name : file_names)
	{
		if (Lower(name) == "icon.sys")
			return true;
	}
	return false;
}

Result ImportFolder(const std::string& card, const std::string& folder_name, const std::vector<FolderFile>& files, const std::string& undo)
{
	if (folder_name.empty() || folder_name.size() > 31)
		return Fail("The save's folder name is not one a card takes (1 to 31 letters).");
	if (files.empty())
		return Fail("The save's folder is empty.");
	uint64_t total = 0;
	const PS5Mc::Stamp now = Now();
	PS5Mc::Save save;
	save.name = folder_name;
	save.mode = PS5Mc::MODE_RWX | PS5Mc::MODE_DIR | PS5Mc::MODE_0400 | PS5Mc::MODE_EXISTS;
	save.created = save.modified = now;
	for (const FolderFile& f : files)
	{
		if (f.name.empty() || f.name.size() > 31 || f.name == "." || f.name == "..")
			return Fail("A file of the save has a name a card does not take: " + f.name);
		total += f.data.size();
		if (total > LARGEST_SAVE_FILE)
			return Fail("The save is larger than any card.");
		PS5Mc::File file;
		file.name = f.name;
		file.mode = PS5Mc::MODE_RWX | PS5Mc::MODE_FILE | PS5Mc::MODE_0400 | PS5Mc::MODE_EXISTS;
		file.created = file.modified = now;
		file.data.assign(f.data.begin(), f.data.end());
		save.files.push_back(std::move(file));
	}
	std::string why;
	Opened opened;
	if (!Open(card, &opened, &why))
		return Fail(why);
	if (!opened.card.Write(save, now, &why))
	{
		if (why == "There is not enough room on the card.")
			why = "There is not enough room on the card: the save needs " + Size(static_cast<uint64_t>(opened.card.ClustersFor(save)) * opened.card.ClusterSize()) +
			      ", and " + Size(opened.card.FreeBytes()) + " are free.";
		return Fail(why);
	}
	if (!Commit(card, opened, undo, &why))
		return Fail(why);
	return Result{true, "\"" + Shown(save) + "\" is on " + Title(card) + " now" + (opened.formatted ? " (the card was new: it was made ready first)" : "") + ". " +
	                        Size(opened.card.FreeBytes()) + " are free."};
}

bool HasUndo(const std::string& card, const std::string& undo)
{
	return !undo.empty() && PS5Backup::PathExists(UndoPath(card, undo));
}

Result Undo(const std::string& card, const std::string& undo)
{
	if (!HasUndo(card, undo))
		return Fail("The card as it was before is not kept.");
	PS5Mc::Bytes kept, now;
	if (!ReadBytes(UndoPath(card, undo), &kept) || kept.empty())
		return Fail("The card as it was before could not be read.");
	// (What is put back has to be a card: one that reads as sound, or one nobody formatted yet.)
	if (!PS5Mc::Card::Unformatted(kept))
	{
		PS5Mc::Card check;
		std::string why;
		if (!check.Open(kept, &why))
			return Fail("The card as it was before does not read as a card: the one there now is left.");
	}
	if (!ReadBytes(card, &now))
		return Fail("The card's file could not be read.");
	// (The one that is there is kept in its turn.)
	std::string why;
	if (!ReplaceCard(card, kept, now, undo, &why))
		return Fail(why);
	return Result{true, Title(card) + " is as it was before the last change."};
}
} // namespace PS5Cards
