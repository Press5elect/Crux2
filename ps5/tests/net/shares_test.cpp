// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// Crux2 - the network shares as Settings > Folders keeps them (frontend/ps5_shares.cpp, built
// with PS5_SHARES_HOST_TEST: no virtual file layer, nothing mounted), the kinds of failure a
// share's test reports (vfs/ps5_share_problem.h), and the socket helper (vfs/ps5_socket.h): a
// socket made non-blocking returns at once, and the port check says open, refused and no answer.
//
//   shares_test <work folder>

#include "ps5_shares.h"
#include "vfs/ps5_share_problem.h"
#include "vfs/ps5_socket.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
int s_checks = 0, s_failures = 0;

void Check(bool ok, const std::string& what)
{
	s_checks++;
	if (!ok)
	{
		s_failures++;
		std::printf("FAILED: %s\n", what.c_str());
	}
}

std::string ReadAll(const std::string& path)
{
	std::string text;
	if (FILE* f = std::fopen(path.c_str(), "r"))
	{
		char buffer[4096];
		size_t n;
		while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
			text.append(buffer, n);
		std::fclose(f);
	}
	return text;
}

void WriteAll(const std::string& path, const std::string& text)
{
	FILE* f = std::fopen(path.c_str(), "w");
	if (f)
	{
		std::fwrite(text.data(), 1, text.size(), f);
		std::fclose(f);
	}
}

void TestFile(const std::string& work)
{
	using namespace PS5Shares;
	const std::string path = work + "/network.cfg";
	// As a user writes it by hand: an empty SMB path (no share, as the layer reads it too), a
	// backslash path, an account of its own, an NFS export with options, a name line.
	WriteAll(path, "\xef\xbb\xbfuser = alice\npassword = p@ss word\nuid = 1026\n"
				   "path = smb://\n"
				   "path = \\\\192.168.1.10\\Games\\PS2\n"
				   "#@ name = Den PC\n"
				   "path = WORK%3Bbob:s%3Acret@nas/media/ps2/\n"
				   "# a comment\n"
				   "path = nfs://192.168.1.20/volume1/games?version=3\n");
	ShareFile file;
	std::string why;
	Check(readShares(path, file, &why), "network.cfg read");
	Check(file.user == "alice" && file.password == "p@ss word" && file.uid == "1026", "the file's account and uid");
	Check(file.shares.size() == 3, "three shares (the empty smb:// line is none, as for the layer)");
	if (file.shares.size() == 3)
	{
		const Share& a = file.shares[0];
		Check(!a.nfs && a.host == "192.168.1.10" && a.path == "Games/PS2" && a.name == "PS2" && a.user.empty(), "a backslash path");
		const Share& b = file.shares[1];
		Check(b.name == "Den PC" && b.host == "nas" && b.user == "WORK;bob" && b.password == "s:cret" && b.path == "media/ps2/",
			"its own account, decoded, and its name");
		const Share& c = file.shares[2];
		Check(c.nfs && c.host == "192.168.1.20" && c.path == "/volume1/games?version=3" && c.name == "games", "an NFS export");
		Check(mountUrl(a, file) == "smb://alice:p%40ss%20word@192.168.1.10/Games/PS2", "an SMB share's address with the file's account");
		Check(mountUrl(b, file) == "smb://WORK;bob:s%3Acret@nas/media/ps2/", "with its own, the ';' kept");
		Check(mountUrl(c, file) == "nfs://192.168.1.20/volume1/games?version=3&uid=1026", "an NFS export with the file's uid");
		Check(shareAddress(b) == "smb://nas/media/ps2/", "the address shown: no account");
	}

	// Written back and read again: the same shares.
	Check(writeShares(path, file, &why), "network.cfg written");
	ShareFile again;
	Check(readShares(path, again, &why) && again.shares.size() == file.shares.size(), "read back");
	for (size_t i = 0; i < again.shares.size() && i < file.shares.size(); i++)
	{
		Check(again.shares[i].name == file.shares[i].name && again.shares[i].host == file.shares[i].host &&
				  again.shares[i].path == file.shares[i].path && again.shares[i].user == file.shares[i].user &&
				  again.shares[i].password == file.shares[i].password && again.shares[i].nfs == file.shares[i].nfs,
			"share " + std::to_string(i) + " the same after writing");
	}
	Check(ReadAll(path).find("#@ name = Den PC") != std::string::npos, "the name line written");
	Check(ReadAll(path + ".new").empty(), "no file left beside it");

	// Mount names: safe, not taken, at most 28.
	Check(shareMountName("Den PC", {}) == "Den_PC", "a space made safe");
	Check(shareMountName("PS2", {"/smb/PS2", "/smb/PS2-2"}) == "PS2-3", "a name taken twice");
	Check(shareMountName("..", {}) == "share", "no '..'");
	Check(shareMountName(std::string(40, 'x'), {}).size() == 28, "at most 28");

	// Nothing given: the test says what is missing before anything is asked.
	Share none;
	Check(testShare(none, file) == PS5SharesProblem::Problem::NoServer, "no server: NoServer");
	none.host = "192.0.2.1";
	none.path = "/";
	Check(testShare(none, file) == PS5SharesProblem::Problem::NoShare, "no share: NoShare");
}

void TestExportsAndNames()
{
	using PS5Shares::exportListed;
	const std::vector<std::string> exports = {"/volume1/games", "/srv/nfs/"};
	Check(exportListed("/volume1/games", exports), "an export");
	Check(exportListed("/volume1/games/ps2?version=3", exports), "a folder inside one, with options");
	Check(exportListed("/srv/nfs", exports), "an export listed with a '/' at its end");
	Check(!exportListed("/volume1/gamesx", exports), "not a folder inside");
	Check(!exportListed("/Volume1/games", exports), "case kept");
	Check(exportListed("/anything", {"/"}), "the root exported");

	// A NetBIOS node status answer: a workgroup (group), the workstation, the file server.
	unsigned char answer[57 + 3 * 18] = {};
	answer[56] = 3;
	const auto entry = [&](int i, const char* name, unsigned char suffix, bool group) {
		unsigned char* e = answer + 57 + i * 18;
		std::memset(e, ' ', 15);
		std::memcpy(e, name, std::strlen(name));
		e[15] = suffix;
		e[16] = group ? 0x80 : 0x00;
	};
	entry(0, "WORKGROUP", 0x00, true);
	entry(1, "DESKTOP-1", 0x00, false);
	entry(2, "DESKTOP-1", 0x20, false);
	Check(PS5Shares::nbstatName(answer, sizeof(answer)) == "DESKTOP-1", "the file server's name");
	answer[57 + 2 * 18 + 15] = 0x03;
	Check(PS5Shares::nbstatName(answer, sizeof(answer)) == "DESKTOP-1", "else the first unique name");
	Check(PS5Shares::nbstatName(answer, 40).empty(), "a short answer: none");
	answer[56] = 200;
	Check(PS5Shares::nbstatName(answer, sizeof(answer)) == "DESKTOP-1", "a count beyond the answer is not read past it");
}

void TestProblems()
{
	using PS5SharesProblem::Classify;
	using PS5SharesProblem::Problem;
	Check(Classify("Session setup failed with (0xc000006d) STATUS_LOGON_FAILURE") == Problem::SignIn, "a refused sign-in");
	Check(Classify("Tree Connect failed with (0xc00000cc) STATUS_BAD_NETWORK_NAME") == Problem::NoSuchShare, "no such share");
	Check(Classify("STATUS_ACCESS_DENIED") == Problem::Denied, "denied");
	Check(Classify("cannot list \"x\": STATUS_OBJECT_PATH_NOT_FOUND") == Problem::NoFolder, "no folder");
	Check(Classify("nothing answers on its SMB port within 5 s (is it on, and on this network?)") == Problem::Unreachable,
		"the port check: no answer");
	Check(Classify("it refuses the SMB port (is file sharing on?)") == Problem::Refused, "the port check: refused");
	Check(Classify("nothing answers on port 2049 (Connection refused)") == Problem::Refused, "the NFS probe: refused");
	Check(Classify("nothing answers on port 2049 (Connection timed out)") == Problem::Unreachable, "the NFS probe: no answer");
	Check(Classify("the address cannot be looked up (give the server's IP address)") == Problem::UnknownName, "no such name");
	Check(Classify("mount: NFS3ERR_ACCES") == Problem::Denied, "an NFS server's no");
	Check(Classify("something new") == Problem::Other, "anything else");
	// A test writes the port check's steps first: an open port's are no failure of their own.
	{
		const std::string steps = "vfs: smb://x: port check: 192.168.1.10:445: non-blocking fcntl 0, FIONBIO 0, SO_NBIO -1; connect "
								  "Operation now in progress after 0 ms; waited 3 ms: poll 1, events 4, SO_ERROR 0: open";
		Check(Classify(steps + "\nvfs: connecting: something new") == Problem::Other, "an open port's steps: no kind of their own");
		Check(Classify(steps + "\nSTATUS_LOGON_FAILURE") == Problem::SignIn, "the server's answer after them");
	}
	for (int p = 0; p <= static_cast<int>(Problem::Other); p++)
		Check(PS5SharesProblem::Text(static_cast<Problem>(p))[0] != 0, "a sentence for every kind");
}

void TestSockets()
{
	// A socket made non-blocking: a read with nothing to read returns at once (EAGAIN).
	int pair[2];
	Check(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "a socket pair");
	const PS5Socket::NonBlocking done = PS5Socket::MakeNonBlocking(pair[0]);
	Check(done.ok() && done.fcntl_result == 0 && done.fionbio_result == 0, "fcntl and FIONBIO took");
	char byte;
	const long long before = PS5Socket::MillisecondsNow();
	const ssize_t got = recv(pair[0], &byte, 1, 0);
	Check(got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK), "an empty read: EAGAIN");
	Check(PS5Socket::MillisecondsNow() - before < 200, "at once");
	PS5Socket::MakeBlocking(pair[0]);
	PS5Socket::SetTimeouts(pair[0], 1);
	const long long waited = PS5Socket::MillisecondsNow();
	Check(recv(pair[0], &byte, 1, 0) < 0, "blocking again: the time limit ends the read");
	Check(PS5Socket::MillisecondsNow() - waited >= 800, "after about a second");
	PS5Socket::NoSigPipe(pair[0]);
	close(pair[0]);
	close(pair[1]);

	// The port check: a listening port, then the same port closed.
	const int listener = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in here{};
	here.sin_family = AF_INET;
	here.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(here);
	if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&here), sizeof(here)) != 0 || listen(listener, 4) != 0 ||
		getsockname(listener, reinterpret_cast<sockaddr*>(&here), &length) != 0)
	{
		std::printf("shares: no TCP port on 127.0.0.1: the port check skipped\n");
		return;
	}
	const int port = ntohs(here.sin_port);
	const PS5Socket::Check open = PS5Socket::CheckPort("127.0.0.1", port, 2000);
	Check(open.open && open.address == "127.0.0.1", "a port that listens: open");
	Check(open.steps.find("connect") != std::string::npos, "its steps written down: " + open.steps);
	close(listener);
	const PS5Socket::Check closed = PS5Socket::CheckPort("127.0.0.1", port, 2000);
	Check(!closed.open && closed.error == ECONNREFUSED, "a closed port: refused (" + closed.steps + ")");
	const PS5Socket::Check nameless = PS5Socket::CheckPort("no-such-host.invalid", 445, 2000);
	Check(!nameless.open && !nameless.looked_up, "a name that is not there: not looked up");
	PS5Socket::IgnoreSigPipe();
	PS5Socket::IgnoreSigPipe();
}
} // namespace

int main(int argc, char** argv)
{
	const std::string work = argc > 1 ? argv[1] : ".";
	TestFile(work);
	TestExportsAndNames();
	TestProblems();
	TestSockets();
	std::printf("shares: %d checks, %d failed\n", s_checks, s_failures);
	return s_failures ? 1 : 0;
}
