// Crux2 - the texture pack manager's file work (frontend/ps5_texture_pack_store.h), on a PC: packs
// found on made-up places, measured, moved from the console to a drive and to a share, moves
// stopped half-way and failing (the source as it was), begun again, deleted, and the downloads'
// queue.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
//   store_test <work folder>     (made empty first)

#include "ps5_texture_pack_store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace PS5TexturePackStore;

namespace
{
int g_checks = 0, g_failed = 0;
void Expect(bool ok, const std::string& what)
{
	g_checks++;
	if (!ok)
	{
		g_failed++;
		std::fprintf(stderr, "FAILED: %s\n", what.c_str());
	}
}

void Write(const fs::path& path, size_t bytes, char fill)
{
	fs::create_directories(path.parent_path());
	std::ofstream out(path, std::ios::binary);
	const std::string data(bytes, fill);
	out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

// A pack of `n` files in a few folders, of sizes that differ.
void MakePack(const fs::path& replacements, int n, char fill)
{
	for (int i = 0; i < n; i++)
	{
		const fs::path sub = replacements / ("part" + std::to_string(i % 3)) / ((i % 2) ? "deeper" : "");
		Write(sub / ("tex" + std::to_string(i) + ".png"), 1000 + static_cast<size_t>(i) * 137, fill);
	}
}

// What a tree holds: each file's path (relative) and size.
std::map<std::string, uintmax_t> Tree(const fs::path& root)
{
	std::map<std::string, uintmax_t> out;
	if (!fs::exists(root))
		return out;
	if (fs::is_regular_file(root))
	{
		out[""] = fs::file_size(root);
		return out;
	}
	for (const auto& e : fs::recursive_directory_iterator(root))
	{
		if (e.is_regular_file())
			out[fs::relative(e.path(), root).string()] = e.file_size();
	}
	return out;
}

// The POSIX calls, with what a test wants of them: a place that cannot rename (a share), a copy
// that fails at a file, a copy that comes out short, copies counted.
class TestFiles : public PosixFiles
{
public:
	std::string noRename;  // paths under this cannot be renamed
	std::string failAt;    // a copy of a file of this name fails
	std::string shortAt;   // a copy of a file of this name comes out a byte short
	int copies = 0;
	int stopAfter = -1;    // the stop says yes after this many copies

	bool CanRename(const std::string& path) override { return noRename.empty() || path.compare(0, noRename.size(), noRename) != 0; }
	bool Rename(const std::string& from, const std::string& to) override
	{
		if (!CanRename(from))
			return false;
		return PosixFiles::Rename(from, to);
	}
	bool CopyFile(const std::string& from, const std::string& to, uint64_t size, const std::function<bool()>& stop,
		const std::function<void(uint64_t)>& moved, std::string* error) override
	{
		if (!failAt.empty() && fs::path(from).filename() == failAt)
		{
			*error = "made to fail";
			return false;
		}
		copies++;
		const bool ok = PosixFiles::CopyFile(from, to, size, stop, moved, error);
		if (ok && !shortAt.empty() && fs::path(from).filename() == shortAt)
			fs::resize_file(to, size - 1);
		return ok;
	}
	std::function<bool()> Stopper()
	{
		return [this] { return stopAfter >= 0 && copies >= stopAfter; };
	}
};

const Pack* FindPack(const std::vector<Pack>& packs, const std::string& serial, size_t place, Pack::State state = Pack::Whole)
{
	for (const Pack& p : packs)
	{
		if (p.serial == serial && p.place == place && p.state == state)
			return &p;
	}
	return nullptr;
}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: store_test <work folder>\n");
		return 2;
	}
	const fs::path work = fs::absolute(argv[1]);
	fs::remove_all(work);
	fs::create_directories(work);
	const fs::path console = work / "console" / "textures";
	const fs::path usb = work / "usb0";
	const fs::path share = work / "smb" / "games";
	fs::create_directories(console);
	fs::create_directories(usb);
	fs::create_directories(share);

	// ---- the places, as the console has them
	std::vector<Place> places;
	{
		Place own;
		own.name = "This console";
		own.top = console.string();
		own.textures = {console.string()};
		own.target = console.string();
		places.push_back(own);
		places.push_back(PlaceAt("USB drive 1", usb.string()));
		places.push_back(PlaceAt("Share: games", share.string() + "/"));
	}
	Expect(places[1].textures.size() == 4 && places[1].target == (usb / "Crux2" / "textures").string(), "a drive's textures folders, Crux2/textures first");
	Expect(places[2].top == share.string(), "a place's top without its last slash");

	Expect(SerialOf("SLUS-21376") == std::optional<std::string>("SLUS-21376"), "a serial is a serial");
	Expect(SerialOf("slus-21376") == std::optional<std::string>("SLUS-21376"), "in any case");
	Expect(!SerialOf("SLUS_21376") && !SerialOf("notes") && !SerialOf("SLUS-2137") && !SerialOf("SLUS-213765"), "what is not a serial is not");

	// ---- found
	MakePack(console / "SLUS-20001" / "replacements", 12, 'a');
	MakePack(usb / "Crux2" / "textures" / "SLUS-20001" / "replacements", 5, 'b');      // the same game on the drive
	MakePack(usb / "PS2" / "textures" / "SCES-50002" / "replacements", 7, 'c');        // in PS2/textures
	Write(usb / "textures" / "SLES-50003.zip", 4096, 'z');                              // a ZIP at the top
	fs::create_directories(console / "SLUS-20009" / "dumps");                           // dumps only: no pack
	Write(console / "notes" / "replacements" / "x.png", 10, 'n');                       // no serial: no pack
	Write(console / "readme.txt", 10, 'r');
	{
		TestFiles files;
		std::vector<Pack> packs = Find(files, places);
		Expect(packs.size() == 4, "four packs found (" + std::to_string(packs.size()) + ")");
		const Pack* own = FindPack(packs, "SLUS-20001", 0);
		const Pack* onUsb = FindPack(packs, "SLUS-20001", 1);
		const Pack* ps2 = FindPack(packs, "SCES-50002", 1);
		Expect(own && own->read, "the console's pack is the one read");
		Expect(onUsb && !onUsb->read, "the drive's copy of the same game is not read");
		Expect(ps2 && ps2->read && ps2->textures == (usb / "PS2" / "textures").string(), "a pack in PS2/textures is found and read");
		bool zipFound = false;
		for (const Pack& p : packs)
			zipFound |= (p.zip && p.serial == "SLES-50003" && p.read && p.files == 1 && p.bytes == 4096);
		Expect(zipFound, "a ZIP is a pack of one file, read");
		Expect(packs.front().place == 0, "the console's packs come first");

		// ---- measured
		const Measured m = Measure(files, own->path, nullptr);
		const auto tree = Tree(own->path);
		uintmax_t bytes = 0;
		for (const auto& [name, size] : tree)
			bytes += size;
		Expect(m.ok && m.files == 12 && m.bytes == bytes, "a pack measured: its files and their bytes");
		int asked = 0;
		const Measured stopped = Measure(files, own->path, [&asked] { return ++asked > 2; });
		Expect(!stopped.ok, "a measure stopped says so");
		Expect(!Measure(files, (console / "nothing").string(), nullptr).ok, "nothing there is not measured");
	}

	// ---- a move from the console to the drive, whole
	MakePack(console / "SCUS-90003" / "replacements", 30, 'd');
	{
		TestFiles files;
		const auto before = Tree(console / "SCUS-90003" / "replacements");
		Pack pack = *FindPack(Find(files, places), "SCUS-90003", 0);
		Progress progress;
		const Outcome o = Move(files, pack, places[1].target, nullptr, &progress);
		const fs::path dest = usb / "Crux2" / "textures" / "SCUS-90003" / "replacements";
		Expect(o.ok && o.error.empty(), "a move to the drive is done: " + o.error);
		Expect(o.path == dest.string(), "and says where the pack is now");
		Expect(Tree(dest) == before, "the drive has every file, of its size");
		Expect(!fs::exists(console / "SCUS-90003"), "the console's copy and its serial's folder are gone");
		Expect(!fs::exists(dest.string() + MOVING), "nothing of the move is left beside it");
		Expect(progress.totalFiles == 30 && o.files == 30, "the move counted its files");
		Expect(FindPack(Find(files, places), "SCUS-90003", 1) != nullptr, "and the pack is found on the drive");
	}

	// ---- a move stopped half-way, then begun again
	MakePack(console / "SLPM-60004" / "replacements", 40, 'e');
	{
		TestFiles files;
		const fs::path src = console / "SLPM-60004" / "replacements";
		const auto before = Tree(src);
		Pack pack = *FindPack(Find(files, places), "SLPM-60004", 0);
		files.stopAfter = 15;
		const Outcome stopped = Move(files, pack, places[1].target, files.Stopper(), nullptr);
		const fs::path dest = usb / "Crux2" / "textures" / "SLPM-60004" / "replacements";
		Expect(!stopped.ok && stopped.stopped, "a move stopped says so");
		Expect(Tree(src) == before, "a move stopped half-way leaves the source as it was");
		Expect(!fs::exists(dest), "and nothing the game would read where it was going");
		Expect(fs::exists(dest.string() + MOVING), "the half copy is beside its place");
		{
			std::vector<Pack> packs = Find(files, places);
			const Pack* half = FindPack(packs, "SLPM-60004", 1, Pack::Moving);
			Expect(half && !half->read, "the half copy is found as an unfinished move, not read");
			Expect(FindPack(packs, "SLPM-60004", 0)->read, "the source is still the one read");
		}
		// (No file of the half copy is half written.)
		bool partial = false;
		for (const auto& [name, size] : Tree(dest.string() + MOVING))
			partial |= name.find(PART) != std::string::npos;
		Expect(!partial, "no half-written file is left of a stopped copy");

		files.stopAfter = -1;
		files.copies = 0;
		const Outcome again = Move(files, pack, places[1].target, files.Stopper(), nullptr);
		Expect(again.ok, "the move begun again is done: " + again.error);
		// (The copy that was under way when the stop came is not kept: 14 were whole.)
		Expect(files.copies == 40 - 14, "it went on from where it stopped (" + std::to_string(files.copies) + " copies)");
		Expect(Tree(dest) == before && !fs::exists(src), "the pack is whole on the drive and gone from the console");
	}

	// ---- a move that fails, and one whose copy comes out wrong: the source as it was
	MakePack(console / "SLES-70005" / "replacements", 10, 'f');
	{
		TestFiles files;
		const fs::path src = console / "SLES-70005" / "replacements";
		const auto before = Tree(src);
		Pack pack = *FindPack(Find(files, places), "SLES-70005", 0);
		files.failAt = "tex6.png";
		const Outcome failed = Move(files, pack, places[1].target, nullptr, nullptr);
		Expect(!failed.ok && !failed.stopped && failed.error == "made to fail", "a copy that fails ends the move, saying why");
		Expect(Tree(src) == before, "a failed move leaves the source as it was");
		files.failAt.clear();
		files.shortAt = "tex3.png";
		fs::remove_all(usb / "Crux2" / "textures" / "SLES-70005");
		const Outcome wrong = Move(files, pack, places[1].target, nullptr, nullptr);
		Expect(!wrong.ok && Tree(src) == before, "a copy not of the file's size is caught, the source kept");
		files.shortAt.clear();
	}

	// ---- a move onto a pack that is there already is refused
	{
		TestFiles files;
		Pack pack = *FindPack(Find(files, places), "SLUS-20001", 0);
		const auto before = Tree(pack.path);
		const Outcome o = Move(files, pack, places[1].target, nullptr, nullptr);
		Expect(!o.ok && o.error.find("there already") != std::string::npos, "a move onto another copy of the pack is refused");
		Expect(Tree(pack.path) == before, "and changes nothing");
		const Outcome same = Move(files, pack, places[0].target, nullptr, nullptr);
		Expect(!same.ok, "a move to where the pack is, is refused");
	}

	// ---- a move to a share, which cannot rename: the copy in its place, stopped, begun again
	MakePack(console / "SLUS-80006" / "replacements", 20, 'g');
	{
		TestFiles files;
		files.noRename = share.string();
		const fs::path src = console / "SLUS-80006" / "replacements";
		const auto before = Tree(src);
		Pack pack = *FindPack(Find(files, places), "SLUS-80006", 0);
		files.stopAfter = 8;
		const Outcome stopped = Move(files, pack, places[2].target, files.Stopper(), nullptr);
		const fs::path dest = share / "Crux2" / "textures" / "SLUS-80006" / "replacements";
		Expect(!stopped.ok && Tree(src) == before, "a move to a share stopped leaves the source as it was");
		Expect(fs::exists(dest) && !fs::exists(dest.string() + MOVING), "on a share the copy is made in its place");
		Expect(fs::is_directory(dest.string() + COPYING), "marked as not whole");
		{
			std::vector<Pack> packs = Find(files, places);
			const Pack* half = FindPack(packs, "SLUS-80006", 2, Pack::Moving);
			Expect(half && !half->read && !FindPack(packs, "SLUS-80006", 2), "the half copy on the share is found as unfinished, not as a pack");
		}
		files.stopAfter = -1;
		files.copies = 0;
		const Outcome again = Move(files, pack, places[2].target, nullptr, nullptr);
		Expect(again.ok && files.copies == 20 - 7, "and goes on from where it stopped: " + again.error);
		Expect(Tree(dest) == before && !fs::exists(src), "the pack is whole on the share, gone from the console");
		Expect(!fs::exists(dest.string() + COPYING), "and its mark is gone");
		// And back from the share (it cannot be renamed away there: deleted in place).
		Pack back = *FindPack(Find(files, places), "SLUS-80006", 2);
		const Outcome home = Move(files, back, places[0].target, nullptr, nullptr);
		Expect(home.ok && Tree(src) == before && !fs::exists(share / "Crux2" / "textures" / "SLUS-80006"), "and moved back from the share");
	}

	// ---- a move onto a share that has a pack of the serial is refused (not merged); an unfinished
	// copy there is deleted with its mark
	MakePack(console / "SLUS-80008" / "replacements", 6, 'i');
	MakePack(share / "Crux2" / "textures" / "SLUS-80008" / "replacements", 3, 'j');
	{
		TestFiles files;
		files.noRename = share.string();
		const fs::path src = console / "SLUS-80008" / "replacements";
		const fs::path there = share / "Crux2" / "textures" / "SLUS-80008" / "replacements";
		const auto before = Tree(src);
		const auto thereBefore = Tree(there);
		Pack pack = *FindPack(Find(files, places), "SLUS-80008", 0);
		const Outcome o = Move(files, pack, places[2].target, nullptr, nullptr);
		Expect(!o.ok && o.error.find("there already") != std::string::npos, "a move onto a share's pack is refused");
		Expect(Tree(src) == before && Tree(there) == thereBefore && !fs::exists(there.string() + COPYING), "and changes nothing");
		fs::remove_all(there);
		files.stopAfter = 2;
		Expect(!Move(files, pack, places[2].target, files.Stopper(), nullptr).ok, "a move to the share stopped");
		const std::vector<Pack> found = Find(files, places);
		const Pack* half = FindPack(found, "SLUS-80008", 2, Pack::Moving);
		Expect(half && Delete(files, *half, nullptr, nullptr).ok && !fs::exists(share / "Crux2" / "textures" / "SLUS-80008"),
			"its unfinished copy is deleted, mark and all");
		Expect(Tree(src) == before, "the source as it was");
		fs::remove_all(console / "SLUS-80008");
	}

	// ---- a ZIP moved, and deleted
	{
		TestFiles files;
		Pack zip;
		for (const Pack& p : Find(files, places))
			if (p.zip)
				zip = p;
		const Outcome o = Move(files, zip, places[0].target, nullptr, nullptr);
		Expect(o.ok && fs::file_size(console / "SLES-50003.zip") == 4096 && !fs::exists(usb / "textures" / "SLES-50003.zip"), "a ZIP is moved");
		zip = *FindPack(Find(files, places), "SLES-50003", 0);
		const Outcome d = Delete(files, zip, nullptr, nullptr);
		Expect(d.ok && !fs::exists(console / "SLES-50003.zip"), "a ZIP is deleted");
	}

	// ---- deleted: stopped half-way (nothing read any more), finished by the next delete
	{
		TestFiles files;
		Pack pack = *FindPack(Find(files, places), "SLUS-20001", 0);
		Progress progress;
		int asked = 0;
		const Outcome stopped = Delete(files, pack, [&asked] { return ++asked > 5; }, &progress);
		Expect(!stopped.ok && stopped.stopped, "a delete stopped says so");
		Expect(!fs::exists(console / "SLUS-20001" / "replacements"), "a pack being deleted is not read from the first moment");
		std::vector<Pack> packs = Find(files, places);
		const Pack* left = FindPack(packs, "SLUS-20001", 0, Pack::Deleting);
		Expect(left != nullptr && !left->read, "what a stopped delete left is found as such");
		Expect(FindPack(packs, "SLUS-20001", 1) && FindPack(packs, "SLUS-20001", 1)->read, "and the drive's copy is the one read now");
		const Outcome done = Delete(files, *left, nullptr, &progress);
		Expect(done.ok && !fs::exists(console / "SLUS-20001"), "the next delete finishes it, and the serial's folder goes");
		Expect(progress.files == 12, "every file of it was counted as deleted");
		// An unfinished move's half copy is deleted the same way.
		MakePack(console / "SLKA-10007" / "replacements.crux2-moving", 4, 'h');
		const std::vector<Pack> found = Find(files, places);
		const Pack* half = FindPack(found, "SLKA-10007", 0, Pack::Moving);
		Expect(half && Delete(files, *half, nullptr, nullptr).ok && !fs::exists(console / "SLKA-10007"), "an unfinished move is deleted");
		Expect(!Move(files, Pack{"X", "", "", 0, false, Pack::Moving}, places[1].target, nullptr, nullptr).ok, "what is not a whole pack is not moved");
	}

	// ---- room
	Expect(Fits(-1, 100) == Room::NotKnown, "no free space known: not known");
	Expect(Fits(int64_t(10) << 30, 2ull << 30) == Room::Fits, "2 GB fit in 10");
	Expect(Fits(int64_t(2) << 30, 2ull << 30) == Room::TooLittle, "2 GB do not fit in 2 (the margin)");
	Expect(UnpackedSize(1000) == 1050, "a pack unpacked takes a little more than its archive");
	{
		PosixFiles files;
		Expect(files.FreeBytes(work.string()) > 0 && files.FreeBytes((work / "nothing").string()) == -1, "a PC knows what is free");
	}

	// ---- the downloads' queue
	{
		Queue<std::string> q;
		Expect(!q.Next(false), "an empty queue starts nothing");
		q.Add("a");
		q.Add("b");
		q.Add("c");
		Expect(!q.Next(true) && q.Size() == 3, "nothing starts while the downloader is busy");
		Expect(q.Has([](const std::string& s) { return s == "b"; }), "what waits is known");
		Expect(q.Remove(1) && !q.Remove(5) && q.Size() == 2, "one is taken off the queue");
		Expect(q.Next(false) == std::optional<std::string>("a") && q.Next(false) == std::optional<std::string>("c") && q.Empty(), "one after another, in order");
	}

	std::printf("texture pack store: %d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
