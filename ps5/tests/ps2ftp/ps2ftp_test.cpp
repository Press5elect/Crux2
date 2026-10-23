// Crux2 - a PlayStation 2's memory cards over FTP (frontend/ps5_ps2_ftp.cpp), against a local
// server that answers as ps2ftpd does (run-ps2ftp-tests.sh starts it).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_ps2_ftp.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int s_checks = 0, s_failed = 0;
static void Expect(bool ok, const std::string& what)
{
	s_checks++;
	if (!ok)
	{
		s_failed++;
		std::printf("FAILED: %s\n", what.c_str());
	}
}

int main(int argc, char** argv)
{
	using namespace PS5Ps2Ftp;
	if (argc != 3)
		return 2;
	const unsigned port = static_cast<unsigned>(std::atoi(argv[1]));
	const std::string mode = argv[2]; // "passive" or "active-only"

	Entry e;
	Expect(ParseListLine("drwxrwxrwx   1 ps2      ps2             0 Sep  8  2013 CBC", &e) && e.folder && e.name == "CBC", "a folder");
	Expect(ParseListLine("-rwxrwxrwx   1 ps2      ps2        380164 Jul 26  2013 boot.elf\r", &e) && !e.folder && e.size == 380164 && e.name == "boot.elf", "a file");
	Expect(ParseListLine("-rw-r--r-- 1 0 0 12 Jan 23 11:44 name with spaces.bin", &e) && e.name == "name with spaces.bin", "spaces in a name");
	Expect(ParseListLine("09-08-13  12:00AM       <DIR>          SAVES", &e) && e.folder && e.name == "SAVES", "MS-DOS folder");
	Expect(ParseListLine("09-08-13  12:00AM                 1234 a.psu", &e) && !e.folder && e.size == 1234, "MS-DOS file");
	Expect(!ParseListLine("total 12", &e) && !ParseListLine("", &e) && !ParseListLine("drwx 1 a b 0 Jan 1 2000 .", &e), "not entries");

	Client client;
	client.SetTimeout(5);
	std::string why;
	Expect(!client.Connect("not an address", port, &why) && !why.empty(), "a name that is no address");
	Expect(!client.Connect("127.0.0.1", 9, &why), "nothing answers");
	Expect(client.Connect("127.0.0.1", port, &why), "connected: " + why);
	std::vector<Save> saves;
	Expect(ListSaves(client, &saves, &why), "the cards listed (" + mode + "): " + why);
	Expect(saves.size() == 3, "three saves (a folder without icon.sys is none): " + std::to_string(saves.size()));
	bool black = false;
	for (const Save& s : saves)
	{
		if (s.name == "BASLUS-21376BLACK")
		{
			black = (s.card == "Memory card 1" && s.files == 2 && s.bytes == 964 + 50000);
			std::vector<SaveFile> files;
			Expect(ReadSave(client, s.path, &files, &why), "read: " + why);
			size_t total = 0;
			for (const SaveFile& f : files)
				total += f.data.size();
			Expect(files.size() == 2 && total == 964 + 50000, "every file whole");
		}
	}
	Expect(black, "the save on card 1 as listed");
	std::vector<SaveFile> files;
	Expect(!ReadSave(client, "/mc/0/NO-SUCH-SAVE", &files, &why), "a save that is not there");
	client.Close();

	std::printf("ps2ftp (%s): %d checks, %d failed\n", mode.c_str(), s_checks, s_failed);
	return s_failed ? 1 : 0;
}
