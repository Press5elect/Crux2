// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
// Crux2 - host test of the memory card held in memory (frontend/ps5_card_image.h):
// the same random reads, writes and erases are made on a card image and on a plain copy of the
// file, and after every flush the file must be that copy, byte for byte. Also: nothing is written
// before a flush is due, a flush writes only the pieces that changed, a failed write keeps what it
// could not write, and a file that is too large or cannot be read is not loaded.
#include "ps5_card_image.h"

#include <cstdlib>
#include <random>
#include <string>
#include <unistd.h>

static int checks, failures;
#define CHECK(condition) \
	do \
	{ \
		checks++; \
		if (!(condition)) \
		{ \
			failures++; \
			std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

static std::vector<uint8_t> ReadFile(const std::string& path)
{
	std::vector<uint8_t> data;
	if (std::FILE* fp = std::fopen(path.c_str(), "rb"))
	{
		std::fseek(fp, 0, SEEK_END);
		data.resize(static_cast<size_t>(std::ftell(fp)));
		std::fseek(fp, 0, SEEK_SET);
		if (!data.empty() && std::fread(data.data(), data.size(), 1, fp) != 1)
			data.clear();
		std::fclose(fp);
	}
	return data;
}

int main(int argc, char* argv[])
{
	const std::string path = std::string((argc > 1) ? argv[1] : ".") + "/card_image_test.ps2";
	static constexpr size_t PAGE = 528, ERASE = 528 * 16;
	static constexpr size_t CARD = 1024 * 528 * 2 * 8; // an 8 MB card: 16384 pages

	// the card as PCSX2 makes it: all ones
	std::vector<uint8_t> model(CARD, 0xff);
	{
		std::FILE* fp = std::fopen(path.c_str(), "wb");
		CHECK(fp && std::fwrite(model.data(), model.size(), 1, fp) == 1);
		std::fclose(fp);
	}

	std::FILE* fp = std::fopen(path.c_str(), "r+b");
	CHECK(fp != nullptr);
	PS5CardImage image;
	CHECK(!image.IsLoaded());
	CHECK(!image.Load(nullptr, CARD));
	CHECK(!image.Load(fp, 0));
	CHECK(!image.Load(fp, PS5CardImage::MAX_SIZE + 1));
	CHECK(!image.Load(fp, CARD + 1)); // more than the file has: the read fails, nothing is loaded
	CHECK(!image.IsLoaded());
	CHECK(image.Load(fp, CARD));
	CHECK(image.IsLoaded() && image.GetSize() == CARD && !image.IsDirty());

	// the bounds a read of the file would have
	CHECK(image.InRange(0, PAGE));
	CHECK(image.InRange(CARD - PAGE, PAGE));
	CHECK(!image.InRange(CARD - PAGE + 1, PAGE));
	CHECK(!image.InRange(CARD, 1));
	CHECK(image.InRange(CARD, 0));
	CHECK(!image.InRange(0xffffffffu, PAGE));
	CHECK(!image.InRange(0, CARD + 1));

	// nothing to flush, nothing due
	CHECK(!image.Tick(30, 120));
	CHECK(image.Flush(fp));

	std::mt19937 rng(20261002);
	std::vector<uint8_t> page(PAGE), read_back(PAGE);
	unsigned flushes = 0;
	for (int round = 0; round < 400; round++)
	{
		// a "save": a few erases and a run of page writes, the way a game makes them
		const unsigned operations = 1 + rng() % 40;
		for (unsigned i = 0; i < operations; i++)
		{
			const uint32_t page_index = rng() % (CARD / PAGE);
			if (rng() % 5 == 0)
			{
				const uint32_t address = (page_index / 16) * ERASE;
				image.Fill(0xff, address, ERASE);
				std::memset(model.data() + address, 0xff, ERASE);
			}
			else
			{
				for (uint8_t& byte : page)
					byte = static_cast<uint8_t>(rng());
				const uint32_t address = page_index * PAGE;
				// as PCSX2 does it: the new data ANDed with what is there
				image.Read(read_back.data(), address, PAGE);
				for (size_t n = 0; n < PAGE; n++)
					read_back[n] &= page[n];
				image.Write(read_back.data(), address, PAGE);
				for (size_t n = 0; n < PAGE; n++)
					model[address + n] &= page[n];
			}
			CHECK(image.IsDirty());
		}
		// a frame after the last write: not due
		CHECK(!image.Tick(30, 100000));

		// what the game reads is what it wrote, before any of it is in the file
		const uint32_t probe = (rng() % (CARD / PAGE)) * PAGE;
		image.Read(read_back.data(), probe, PAGE);
		CHECK(std::memcmp(read_back.data(), model.data() + probe, PAGE) == 0);

		// 29 more frames without a write are not yet half a second; the 30th is
		bool due = false;
		for (int frame = 0; frame < 28; frame++)
			due = due || image.Tick(30, 100000);
		CHECK(!due);
		CHECK(image.Tick(30, 100000));

		if (round % 3 == 0)
		{
			CHECK(image.Flush(fp));
			flushes++;
			CHECK(!image.IsDirty());
			CHECK(!image.Tick(30, 120));
			CHECK(ReadFile(path) == model);
		}
	}
	CHECK(image.Flush(fp));
	CHECK(ReadFile(path) == model);

	// a flush writes the pieces that changed and no others: one page changed, and a byte that
	// was changed in the file behind the image's back, in another piece, is left alone
	{
		const uint32_t far_address = 5 * PS5CardImage::CHUNK_SIZE + 17;
		const uint8_t marker = static_cast<uint8_t>(~model[far_address]);
		std::FILE* other = std::fopen(path.c_str(), "r+b");
		CHECK(other && std::fseek(other, far_address, SEEK_SET) == 0 && std::fwrite(&marker, 1, 1, other) == 1);
		std::fclose(other);

		std::memset(page.data(), 0x5a, PAGE);
		image.Write(page.data(), 0, PAGE);
		std::memcpy(model.data(), page.data(), PAGE);
		CHECK(image.Flush(fp));

		std::vector<uint8_t> file = ReadFile(path);
		CHECK(file.size() == CARD && file[far_address] == marker);
		file[far_address] = model[far_address];
		CHECK(file == model);

		// put it back for the comparisons below
		image.Write(model.data() + far_address, far_address, 1);
		CHECK(image.Flush(fp));
		CHECK(ReadFile(path) == model);
	}

	// the card's last page
	{
		std::memset(page.data(), 0x33, PAGE);
		image.Write(page.data(), static_cast<uint32_t>(CARD - PAGE), PAGE);
		std::memcpy(model.data() + CARD - PAGE, page.data(), PAGE);
		CHECK(image.Flush(fp));
		const std::vector<uint8_t> file = ReadFile(path);
		CHECK(file.size() == CARD);
		CHECK(file == model);
	}

	// a game that never leaves its card alone: a write on every frame keeps the idle count at
	// zero, and the card is still due once its oldest change is two seconds old
	{
		std::memset(page.data(), 0x11, PAGE);
		bool due = false;
		int frame = 0;
		for (; frame < 1000 && !due; frame++)
		{
			image.Write(page.data(), 7 * PAGE, PAGE);
			due = image.Tick(30, 120);
		}
		std::memcpy(model.data() + 7 * PAGE, page.data(), PAGE);
		CHECK(due && frame == 120);
		CHECK(image.Flush(fp));
		CHECK(ReadFile(path) == model);
		// and the age starts again with the next change
		image.Write(page.data(), 9 * PAGE, PAGE);
		std::memcpy(model.data() + 9 * PAGE, page.data(), PAGE);
		CHECK(!image.Tick(30, 120));
		CHECK(image.Flush(fp));
	}

	// cards whose size is no multiple of the 64 KiB piece (a PS1 card with a header, a file cut
	// short): the last, partial piece is written whole and nothing after it
	for (const size_t odd_size : {size_t(0x20000 + 528), size_t(PS5CardImage::CHUNK_SIZE - 1), size_t(1),
			 size_t(3 * PS5CardImage::CHUNK_SIZE + 1)})
	{
		const std::string odd_path = path + ".odd";
		std::vector<uint8_t> odd_model(odd_size);
		for (uint8_t& byte : odd_model)
			byte = static_cast<uint8_t>(rng());
		std::FILE* odd = std::fopen(odd_path.c_str(), "w+b");
		CHECK(odd && std::fwrite(odd_model.data(), odd_model.size(), 1, odd) == 1 && std::fflush(odd) == 0);

		PS5CardImage odd_image;
		CHECK(odd_image.Load(odd, static_cast<int64_t>(odd_size)));
		CHECK(odd_image.GetSize() == odd_size);
		CHECK(odd_image.InRange(static_cast<uint32_t>(odd_size - 1), 1));
		CHECK(!odd_image.InRange(static_cast<uint32_t>(odd_size), 1));
		const uint8_t last = static_cast<uint8_t>(~odd_model[odd_size - 1]);
		odd_image.Write(&last, static_cast<uint32_t>(odd_size - 1), 1);
		odd_model[odd_size - 1] = last;
		odd_image.Fill(0xa5, 0, 1);
		odd_model[0] = 0xa5;
		CHECK(odd_image.Flush(odd));
		std::fclose(odd);
		CHECK(ReadFile(odd_path) == odd_model);
		unlink(odd_path.c_str());
	}

	// a file that cannot be written: the changes stay, and reach the file once it can be
	{
		std::FILE* read_only = std::fopen(path.c_str(), "rb");
		CHECK(read_only != nullptr);
		std::memset(page.data(), 0x77, PAGE);
		image.Write(page.data(), 3 * PAGE, PAGE);
		std::memcpy(model.data() + 3 * PAGE, page.data(), PAGE);
		CHECK(!image.Flush(read_only));
		CHECK(image.IsDirty());
		CHECK(!image.Flush(nullptr));
		CHECK(image.IsDirty());
		std::fclose(read_only);
		// a failed flush starts the wait again: not tried on every frame
		CHECK(!image.Tick(30, 120));
		CHECK(ReadFile(path) != model);
		CHECK(image.Flush(fp));
		CHECK(!image.IsDirty());
		CHECK(ReadFile(path) == model);
	}

	image.Unload();
	CHECK(!image.IsLoaded() && !image.IsDirty() && image.GetSize() == 0);
	std::fclose(fp);
	unlink(path.c_str());

	std::printf("memory card image: %d checks, %d failed (%u flushes compared with the file)\n", checks, failures, flushes);
	return failures ? 1 : 0;
}
