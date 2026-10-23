// Crux2 - the card manager's engine (frontend/ps5_cards.cpp), on the PC, with card files and save
// files in a folder of the PC.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   cards_engine_test <work folder>
//
// What is checked: what a change says, what the card holds afterwards, that the card as it was
// before is kept and can be put back, and that a change that is refused leaves every file as it
// was, byte for byte.

#include "ps5_backup.h"
#include "ps5_cards.h"
#include "ps5_mc.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static int s_checks = 0, s_failed = 0;
#define CHECK(what, ...) \
	do \
	{ \
		s_checks++; \
		if (!(what)) \
		{ \
			s_failed++; \
			std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); \
			__VA_OPT__(std::printf("       " __VA_ARGS__); std::printf("\n");) \
		} \
	} while (0)

static std::mt19937 s_random(41);

static std::string Get(const std::string& path)
{
	std::string data;
	return PS5Backup::ReadWholeFile(path, &data) ? data : std::string("<missing>");
}
static void Put(const std::string& path, const std::string& data)
{
	if (!PS5Backup::WriteWholeFile(path, data))
	{
		std::printf("cannot write %s\n", path.c_str());
		std::exit(2);
	}
}

static PS5Mc::Save MakeSave(const std::string& name, const std::string& title, size_t size)
{
	PS5Mc::Save save;
	save.name = name;
	save.mode = PS5Mc::MODE_RWX | PS5Mc::MODE_DIR | PS5Mc::MODE_0400 | PS5Mc::MODE_EXISTS;
	save.created = save.modified = PS5Mc::Stamp::Of(2005, 6, 7, 8, 9, 10);
	PS5Mc::File icon;
	icon.name = "icon.sys";
	icon.mode = PS5Mc::MODE_RWX | PS5Mc::MODE_FILE | PS5Mc::MODE_0400 | PS5Mc::MODE_EXISTS;
	icon.created = icon.modified = save.created;
	icon.data.assign(964, 0);
	std::memcpy(icon.data.data(), "PS2D", 4);
	for (size_t i = 0; i < title.size() && i < 32; i++)
	{
		const char c = title[i];
		const unsigned code = (c >= 'A' && c <= 'Z') ? 0x8260u + static_cast<unsigned>(c - 'A') : (c >= 'a' && c <= 'z') ? 0x8281u + static_cast<unsigned>(c - 'a') : 0x8140u;
		icon.data[0xc0 + i * 2] = static_cast<uint8_t>(code >> 8);
		icon.data[0xc0 + i * 2 + 1] = static_cast<uint8_t>(code);
	}
	PS5Mc::detail::Put16(icon.data.data() + 6, static_cast<uint16_t>(title.size() * 2));
	save.files.push_back(icon);
	PS5Mc::File data = icon;
	data.name = "DATA.BIN";
	data.data.resize(size);
	for (uint8_t& b : data.data)
		b = static_cast<uint8_t>(s_random());
	save.files.push_back(std::move(data));
	return save;
}

static std::string Psu(const PS5Mc::Save& save)
{
	const PS5Mc::Bytes bytes = PS5Mc::WritePSU(save);
	return std::string(bytes.begin(), bytes.end());
}

/// Whether a folder holds a file whose name ends so (a write that was given up half way).
static bool AnyEndingWith(const std::string& folder, const char* ending)
{
	bool any = false;
	if (DIR* const dir = opendir(folder.c_str()))
	{
		while (const struct dirent* const entry = readdir(dir))
		{
			const std::string name = entry->d_name;
			any |= name.size() > std::strlen(ending) && name.compare(name.size() - std::strlen(ending), std::string::npos, ending) == 0;
		}
		closedir(dir);
	}
	return any;
}

int main(int argc, char** argv)
{
	if (argc != 2)
		return 2;
	const std::string work = argv[1], cards = work + "/memcards", saves = work + "/saves", undo = work + "/card-undo", out = work + "/out";
	std::system(("rm -rf '" + cards + "' '" + saves + "' '" + undo + "' '" + out + "'").c_str());
	PS5Backup::MakeAllFolders(cards);
	PS5Backup::MakeAllFolders(saves);

	const std::string new_card(PS5Mc::Card::SIZE_8MB_ECC, '\xff');
	const std::string one = cards + "/Card 1.ps2", two = cards + "/Card 2.ps2";
	Put(one, new_card);
	Put(two, new_card);

	// A card nobody formatted: empty, with the room it will have.
	{
		const PS5Cards::CardInfo info = PS5Cards::Look(one);
		CHECK(info.state == PS5Cards::CardInfo::New && info.saves.empty());
		CHECK(info.total_bytes == 8135u * 1024u && info.free_bytes == 8134u * 1024u, "%u, %u", info.total_bytes, info.free_bytes);
		CHECK(Get(one) == new_card); // (looking changes nothing)
		CHECK(!PS5Cards::HasUndo(one, undo));
	}

	// The save files of a folder: those of the kinds that are read, by name.
	const PS5Mc::Save first = MakeSave("BESLES-50001SAVE", "First", 40000), second = MakeSave("BASLUS-20002:A?", "Second", 9000);
	Put(saves + "/b second.PSU", Psu(second));
	Put(saves + "/a first.psu", Psu(first));
	Put(saves + "/notes.txt", "not a save");
	Put(saves + "/.hidden.psu", Psu(first));
	Put(saves + "/empty.max", "");
	Put(saves + "/noise.cbs", std::string(5000, 'x'));
	PS5Backup::MakeAllFolders(saves + "/folder.psu");
	{
		const std::vector<PS5Cards::SaveFile> files = PS5Cards::SaveFiles(saves);
		CHECK(files.size() == 3, "%zu", files.size());
		CHECK(files.size() == 3 && files[0].name == "a first.psu" && files[1].name == "b second.PSU" && files[2].name == "noise.cbs");
		CHECK(files.size() == 3 && files[0].path == saves + "/a first.psu" && files[0].size == Psu(first).size());
		CHECK(PS5Cards::SaveFiles(work + "/no such folder").empty());
		CHECK(PS5Cards::IsSaveFile("x.xps") && PS5Cards::IsSaveFile("x.PsV") && !PS5Cards::IsSaveFile("psu") && !PS5Cards::IsSaveFile("x.ps2"));
	}

	// Brought in: the new card is made ready, the save is on it, the card as it was is kept.
	{
		const PS5Cards::Result result = PS5Cards::Import(one, saves + "/a first.psu", undo);
		CHECK(result.ok, "%s", result.message.c_str());
		CHECK(result.message == "\"First\" is on Card 1 now (the card was new: it was made ready first). 7.9 MB are free.", "%s", result.message.c_str());
		const PS5Cards::CardInfo info = PS5Cards::Look(one);
		CHECK(info.state == PS5Cards::CardInfo::Listed && info.saves.size() == 1);
		CHECK(info.saves.size() == 1 && info.saves[0].name == "BESLES-50001SAVE" && info.saves[0].title == "First" && info.saves[0].files == 2 &&
			  info.saves[0].when == "2005-06-07 08:09" && info.saves[0].bytes == 44032,
			"%s", info.saves.empty() ? "?" : (info.saves[0].title + " " + info.saves[0].when + " " + std::to_string(info.saves[0].bytes)).c_str());
		CHECK(PS5Cards::HasUndo(one, undo) && Get(undo + "/Card 1.ps2") == new_card);
	}
	// The same again: refused, and nothing is another file than before.
	{
		const std::string card = Get(one);
		Put(undo + "/Card 1.ps2", "what was kept");
		const PS5Cards::Result result = PS5Cards::Import(one, saves + "/a first.psu", undo);
		CHECK(!result.ok && result.message == "A save of that name is on the card already.", "%s", result.message.c_str());
		CHECK(Get(one) == card && Get(undo + "/Card 1.ps2") == "what was kept");
		// What is no save, and what is not there.
		PS5Cards::Result bad = PS5Cards::Import(one, saves + "/noise.cbs", undo);
		CHECK(!bad.ok && bad.message.find("none of the save files") != std::string::npos, "%s", bad.message.c_str());
		bad = PS5Cards::Import(one, saves + "/gone.psu", undo);
		CHECK(!bad.ok && bad.message == "The save file could not be read.", "%s", bad.message.c_str());
		bad = PS5Cards::Import(cards + "/gone.ps2", saves + "/a first.psu", undo);
		CHECK(!bad.ok && bad.message == "The card's file could not be read.", "%s", bad.message.c_str());
		CHECK(Get(one) == card && Get(undo + "/Card 1.ps2") == "what was kept");
		Put(undo + "/Card 1.ps2", new_card);
	}
	// A second save, whose name no drive takes as a file's.
	{
		const PS5Cards::Result result = PS5Cards::Import(one, saves + "/b second.PSU", undo);
		CHECK(result.ok && result.message == "\"Second\" is on Card 1 now. 7.9 MB are free.", "%s", result.message.c_str());
		CHECK(PS5Cards::Look(one).saves.size() == 2);
	}

	// Written out: the file reads as the save that went in; a second one is numbered.
	{
		PS5Cards::Result result = PS5Cards::Export(one, "BESLES-50001SAVE", out);
		CHECK(result.ok && result.message == "\"First\" was written as BESLES-50001SAVE.psu.", "%s", result.message.c_str());
		PS5Mc::Save back;
		std::string why;
		const std::string file = Get(out + "/BESLES-50001SAVE.psu");
		CHECK(PS5Mc::ReadSaveFile(PS5Mc::Bytes(file.begin(), file.end()), &back, PS5Mc::Stamp(), &why), "%s", why.c_str());
		CHECK(back.name == first.name && back.files.size() == 2 && back.files[1].data == first.files[1].data);
		result = PS5Cards::Export(one, "BESLES-50001SAVE", out);
		CHECK(result.ok && result.message == "\"First\" was written as BESLES-50001SAVE (2).psu.", "%s", result.message.c_str());
		CHECK(Get(out + "/BESLES-50001SAVE (2).psu") == file);
		result = PS5Cards::Export(one, "BASLUS-20002:A?", out);
		CHECK(result.ok && result.message == "\"Second\" was written as BASLUS-20002_A_.psu.", "%s", result.message.c_str());
		result = PS5Cards::Export(one, "NOTTHERE", out);
		CHECK(!result.ok && result.message == "That save is not on the card.", "%s", result.message.c_str());
		// (And what was written out comes in again, on another card.)
		result = PS5Cards::Import(two, out + "/BASLUS-20002_A_.psu", undo);
		CHECK(result.ok, "%s", result.message.c_str());
		CHECK(PS5Cards::Look(two).saves.size() == 1 && PS5Cards::Look(two).saves[0].name == "BASLUS-20002:A?");
	}

	// From card to card.
	{
		const std::string before_one = Get(one);
		PS5Cards::Result result = PS5Cards::Copy(one, "BESLES-50001SAVE", two, undo);
		CHECK(result.ok && result.message == "\"First\" is on Card 2 too now.", "%s", result.message.c_str());
		CHECK(Get(one) == before_one);
		CHECK(PS5Cards::Look(two).saves.size() == 2);
		const std::string before_two = Get(two);
		result = PS5Cards::Copy(one, "BESLES-50001SAVE", two, undo);
		CHECK(!result.ok && result.message == "A save of that name is on Card 2 already: delete it there first to put this one in its place.", "%s", result.message.c_str());
		result = PS5Cards::Copy(one, "BESLES-50001SAVE", one, undo);
		CHECK(!result.ok && result.message == "That is the card the save is on.", "%s", result.message.c_str());
		result = PS5Cards::Copy(one, "NOTTHERE", two, undo);
		CHECK(!result.ok);
		CHECK(Get(one) == before_one && Get(two) == before_two);
	}

	// Deleted, put back, and deleted again by putting back once more.
	{
		const std::string with = Get(two);
		PS5Cards::Result result = PS5Cards::Delete(two, "BESLES-50001SAVE", undo);
		CHECK(result.ok && result.message == "\"First\" was deleted from Card 2. 7.9 MB are free.", "%s", result.message.c_str());
		const std::string without = Get(two);
		CHECK(PS5Cards::Look(two).saves.size() == 1 && Get(undo + "/Card 2.ps2") == with);
		result = PS5Cards::Undo(two, undo);
		CHECK(result.ok && result.message == "Card 2 is as it was before the last change.", "%s", result.message.c_str());
		CHECK(Get(two) == with && Get(undo + "/Card 2.ps2") == without);
		result = PS5Cards::Undo(two, undo);
		CHECK(result.ok && Get(two) == without && Get(undo + "/Card 2.ps2") == with);
		result = PS5Cards::Delete(two, "NOTTHERE", undo);
		CHECK(!result.ok && result.message == "That save is not on the card.", "%s", result.message.c_str());
		CHECK(Get(two) == without);
		// What is kept is no card: it is not put back.
		Put(undo + "/Card 2.ps2", std::string(5000, 'z'));
		result = PS5Cards::Undo(two, undo);
		CHECK(!result.ok && result.message == "The card as it was before does not read as a card: the one there now is left.", "%s", result.message.c_str());
		CHECK(Get(two) == without);
		CHECK(!PS5Cards::Undo(cards + "/Card 9.ps2", undo).ok);
	}

	// A save too large for what is left: said with the numbers, the card as it was.
	{
		const PS5Mc::Save large = MakeSave("BESLES-59999BIG", "Large", 5 * 1024 * 1024);
		Put(saves + "/large.psu", Psu(large));
		CHECK(PS5Cards::Import(one, saves + "/large.psu", undo).ok);
		PS5Mc::Save again = large;
		again.name = "BESLES-59998BIG";
		Put(saves + "/large2.psu", Psu(again));
		const std::string before = Get(one), kept = Get(undo + "/Card 1.ps2");
		const PS5Cards::Result result = PS5Cards::Import(one, saves + "/large2.psu", undo);
		CHECK(!result.ok && result.message == "There is not enough room on the card: the save needs 5.0 MB, and 2.9 MB are free.", "%s", result.message.c_str());
		CHECK(Get(one) == before && Get(undo + "/Card 1.ps2") == kept);
	}

	// What is no card, and a card that does not read as sound: looked at, never written to.
	{
		const std::string noise = cards + "/Noise.ps2", damaged = cards + "/Damaged.ps2";
		Put(noise, std::string(100000, 'n'));
		std::string broken = Get(one);
		// (The top directory's first entry says it is a file.)
		broken[41 * 2 * 528] = 0x17;
		Put(damaged, broken);
		PS5Cards::CardInfo info = PS5Cards::Look(noise);
		CHECK(info.state == PS5Cards::CardInfo::Unknown && info.why == "This is not a PlayStation 2 memory card.", "%s", info.why.c_str());
		info = PS5Cards::Look(damaged);
		CHECK(info.state == PS5Cards::CardInfo::Unknown && info.why.find("does not read as sound") != std::string::npos, "%s", info.why.c_str());
		for (const std::string& card : {noise, damaged})
		{
			const std::string before = Get(card);
			CHECK(!PS5Cards::Import(card, saves + "/a first.psu", undo).ok);
			CHECK(!PS5Cards::Delete(card, "BESLES-50001SAVE", undo).ok);
			CHECK(!PS5Cards::Export(card, "BESLES-50001SAVE", out).ok);
			CHECK(!PS5Cards::Copy(one, "BESLES-50001SAVE", card, undo).ok);
			CHECK(!PS5Cards::Copy(card, "BESLES-50001SAVE", two, undo).ok);
			CHECK(Get(card) == before && !PS5Cards::HasUndo(card, undo));
		}
		// A file of 0xFF that is of no card's size.
		Put(cards + "/Odd.ps2", std::string(123456, '\xff'));
		info = PS5Cards::Look(cards + "/Odd.ps2");
		CHECK(info.state == PS5Cards::CardInfo::Unknown, "%s", info.why.c_str());
		CHECK(!PS5Cards::Import(cards + "/Odd.ps2", saves + "/a first.psu", undo).ok);
	}

	// Where the card as it was cannot be kept, nothing is changed.
	{
		const std::string blocked = work + "/out/BESLES-50001SAVE.psu/undo"; // (under a file: no folder can be made there)
		const std::string before = Get(two);
		const PS5Cards::Result result = PS5Cards::Delete(two, "BASLUS-20002:A?", blocked);
		CHECK(!result.ok && result.message == "The card as it was could not be kept: nothing was changed.", "%s", result.message.c_str());
		CHECK(Get(two) == before);
		// With no folder named, none is kept and the change is made.
		CHECK(PS5Cards::Delete(two, "BASLUS-20002:A?", std::string()).ok);
		CHECK(PS5Cards::Look(two).saves.empty());
	}

	// A game was started while a change was on its way: the card is left, and nothing is kept.
	{
		bool allowed = false;
		PS5Cards::SetWriteGuard([&allowed] { return allowed; });
		const std::string before = Get(one), kept = Get(undo + "/Card 1.ps2");
		PS5Cards::Result result = PS5Cards::Delete(one, "BESLES-50001SAVE", undo);
		CHECK(!result.ok && result.message == "A game was started meanwhile: the card is as it was.", "%s", result.message.c_str());
		result = PS5Cards::Undo(one, undo);
		CHECK(!result.ok && result.message == "A game was started meanwhile: the card is as it was.", "%s", result.message.c_str());
		CHECK(Get(one) == before && Get(undo + "/Card 1.ps2") == kept);
		// (Writing a save out changes no card: it is not held back.)
		CHECK(PS5Cards::Export(one, "BESLES-50001SAVE", out).ok);
		allowed = true;
		CHECK(PS5Cards::Delete(one, "BESLES-50001SAVE", undo).ok);
		PS5Cards::WaitForWrites();
		PS5Cards::SetWriteGuard({});
	}

	// v1.5.0: whole cards, formatting, and saves that come as folders.
	{
		const std::string made = cards + "/Made.ps2";
		CHECK(PS5Cards::Create(made, 8).ok);
		CHECK(!PS5Cards::Create(made, 8).ok, "a card that is there is not made again");
		CHECK(!PS5Cards::Create(cards + "/Odd size.ps2", 12).ok);
		PS5Cards::CardInfo info = PS5Cards::Look(made);
		CHECK(info.state == PS5Cards::CardInfo::Listed && info.saves.empty() && Get(made).size() == PS5Mc::Card::SIZE_8MB_ECC, "%s", info.why.c_str());
		CHECK(PS5Cards::Create(cards + "/Big.ps2", 64).ok && Get(cards + "/Big.ps2").size() == 8 * PS5Mc::Card::SIZE_8MB_ECC);

		// A save as a folder of files.
		const PS5Mc::Save folder_save = MakeSave("BASLUS-21376BLACK", "Black", 30000);
		std::vector<PS5Cards::FolderFile> files;
		std::vector<std::string> names;
		for (const PS5Mc::File& f : folder_save.files)
		{
			files.push_back({f.name, std::string(f.data.begin(), f.data.end())});
			names.push_back(f.name);
		}
		CHECK(PS5Cards::LooksLikeSaveFolder(names) && !PS5Cards::LooksLikeSaveFolder({"DATA.BIN"}));
		PS5Cards::Result result = PS5Cards::ImportFolder(made, "BASLUS-21376BLACK", files, undo);
		CHECK(result.ok, "%s", result.message.c_str());
		info = PS5Cards::Look(made);
		CHECK(info.saves.size() == 1 && info.saves[0].name == "BASLUS-21376BLACK" && info.saves[0].title == "Black");
		CHECK(!PS5Cards::ImportFolder(made, "BASLUS-21376BLACK", files, undo).ok, "the same save twice is refused");
		CHECK(!PS5Cards::ImportFolder(made, "", files, undo).ok && !PS5Cards::ImportFolder(made, "EMPTY", {}, undo).ok);

		// Written out whole, and brought in again as a new card.
		result = PS5Cards::ExportCard(made, out);
		CHECK(result.ok && Get(out + "/Made.ps2") == Get(made), "%s", result.message.c_str());
		result = PS5Cards::ImportCard(out + "/Made.ps2", cards + "/Again.ps2");
		CHECK(result.ok && Get(cards + "/Again.ps2") == Get(made), "%s", result.message.c_str());
		CHECK(!PS5Cards::ImportCard(out + "/Made.ps2", cards + "/Again.ps2").ok, "a card's name that is taken");

		// A card without the error-correction bytes (a PlayStation 3 .vm2, a dump): given them.
		{
			const std::string ecc = Get(made);
			std::string plain;
			for (size_t page = 0; page < ecc.size() / 528; page++)
				plain += ecc.substr(page * 528, 512);
			Put(out + "/Plain.vm2", plain);
			CHECK(PS5Cards::IsCardImageName("Plain.vm2") && PS5Cards::IsCardImageSize(plain.size()) && !PS5Cards::IsCardImageName("notes.txt"));
			result = PS5Cards::ImportCard(out + "/Plain.vm2", cards + "/From PS3.ps2");
			// (The pages are the same; a page of nothing but 0xFF may carry either spare bytes.)
			const std::string back = Get(cards + "/From PS3.ps2");
			std::string back_plain;
			for (size_t page = 0; page < back.size() / 528; page++)
				back_plain += back.substr(page * 528, 512);
			CHECK(result.ok && back.size() == ecc.size() && back_plain == plain, "%s", result.message.c_str());
			info = PS5Cards::Look(cards + "/From PS3.ps2");
			CHECK(info.state == PS5Cards::CardInfo::Listed && info.saves.size() == 1);
		}
		// What is not a card.
		Put(out + "/Junk.bin", std::string(PS5Mc::Card::SIZE_8MB_ECC, 'x'));
		CHECK(!PS5Cards::ImportCard(out + "/Junk.bin", cards + "/Junk.ps2").ok && !PS5Backup::PathExists(cards + "/Junk.ps2"));
		Put(out + "/Small.bin", std::string(1000, 0));
		CHECK(!PS5Cards::ImportCard(out + "/Small.bin", cards + "/Small.ps2").ok);

		// Formatted: empty, the size kept, the card before kept for undo.
		const std::string before_format = Get(made);
		result = PS5Cards::Format(made, undo);
		CHECK(result.ok, "%s", result.message.c_str());
		info = PS5Cards::Look(made);
		CHECK(info.state == PS5Cards::CardInfo::Listed && info.saves.empty() && Get(made).size() == before_format.size());
		CHECK(PS5Cards::Undo(made, undo).ok && Get(made) == before_format, "format can be undone");
	}

	// No half-written file is left anywhere.
	CHECK(!AnyEndingWith(cards, ".part") && !AnyEndingWith(undo, ".part") && !AnyEndingWith(out, ".part"));
	CHECK(PS5Cards::Size(120 * 1024) == "120 KB" && PS5Cards::Size(7445504) == "7.1 MB");

	std::printf("card manager: %d checks, %d failed\n", s_checks, s_failed);
	return s_failed == 0 ? 0 : 1;
}
