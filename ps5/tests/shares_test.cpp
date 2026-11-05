// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 v1.0.19 - network.cfg as Settings > Folders reads and writes it (ps5/frontend/ps5_shares.cpp;
// from OGBox 0.0.3's tests/shares/shares_test.cpp).
#include "ps5_shares.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace PS5Shares;

static int failures = 0, checks = 0;
#define CHECK(x) do { checks++; if (!(x)) { failures++; std::printf("FAILED line %d: %s\n", __LINE__, #x); } } while (0)

int main(int argc, char **argv)
{
	const std::string f = std::string(argc > 1 ? argv[1] : "/tmp") + "/crux2-shares-test.cfg";
	// The template Crux2 writes, uncommented as a hand would, and the forms the old reader took.
	FILE *o = std::fopen(f.c_str(), "w");
	std::fputs("# path = 192.168.1.10/Games/PS2\nuser = kuba\npassword = p w\n\npath = 192.168.1.10/Games/PS2\n"
		"path = nfs://nas/volume1/ps2?version=4\nuid = 1026\n#@ name = Desk PC\npath = smb://bob:a%40b@desk/share/x\n"
		"path = \\\\pc\\Share\\PS2\npath = OFFICE;ann:pw@10.0.0.9/games\n", o);
	std::fclose(o);
	ShareFile sf;
	CHECK(readShares(f, sf));
	CHECK(sf.shares.size() == 5 && sf.user == "kuba" && sf.password == "p w" && sf.uid == "1026");
	CHECK(sf.shares[0].name == "PS2" && sf.shares[0].host == "192.168.1.10" && sf.shares[0].path == "Games/PS2" && sf.shares[0].user.empty());
	CHECK(sf.shares[1].nfs && sf.shares[1].host == "nas" && sf.shares[1].path == "/volume1/ps2?version=4" && sf.shares[1].name == "ps2");
	CHECK(shareAddress(sf.shares[1]) == "nfs://nas/volume1/ps2");
	CHECK(sf.shares[2].name == "Desk PC" && sf.shares[2].user == "bob" && sf.shares[2].password == "a@b");
	CHECK(sf.shares[3].host == "pc" && sf.shares[3].path == "Share/PS2" && sf.shares[3].name == "PS2");
	CHECK(sf.shares[4].user == "OFFICE;ann" && sf.shares[4].password == "pw");
	// Mount names as the old reader gave them: the last folder, made safe, a second "PS2" as "PS2-2".
	ShareFile mounted = sf;
	std::vector<std::string> taken;
	for (Share &s : mounted.shares) {
		CHECK(mountShare(s, mounted, taken));
		taken.push_back(s.mount);
	}
	CHECK(mounted.shares[0].mount == "/smb/PS2" && mounted.shares[1].mount == "/smb/ps2" && mounted.shares[2].mount == "/smb/Desk_PC" &&
		mounted.shares[3].mount == "/smb/PS2-2" && mounted.shares[4].mount == "/smb/games");
	// Written back and read again: every share as it was, a new one with its own account.
	Share n; n.name = "Syn ology"; n.host = "10.0.0.2"; n.path = "games"; n.user = "me"; n.password = "x:y z";
	sf.shares.push_back(n);
	CHECK(writeShares(f, sf));
	ShareFile back;
	CHECK(readShares(f, back));
	CHECK(back.shares.size() == 6 && back.shares[5].password == "x:y z" && back.shares[5].name == "Syn ology");
	CHECK(back.shares[1].path == "/volume1/ps2?version=4" && back.user == "kuba" && back.uid == "1026");
	CHECK(back.shares[4].user == "OFFICE;ann" && back.shares[2].password == "a@b");
	CHECK(accountEncode("OFFICE;ann b") == "OFFICE;ann%20b");
	CHECK(shareMountName("Syn ology", { "/smb/Syn_ology" }) == "Syn_ology-2");
	// The process's own: loaded, changed, saved.
	CHECK(LoadAndMount(f) == 6 && Current().shares.size() == 6 && CurrentFile() == f);
	Current().shares.erase(Current().shares.begin());
	CHECK(Save());
	ShareFile after;
	CHECK(readShares(f, after) && after.shares.size() == 5 && after.shares[0].nfs);
	std::remove(f.c_str());
	std::printf("network shares: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
