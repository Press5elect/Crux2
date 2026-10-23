// Crux2 - disc images in the formats PCSX2 plays from (see ps5_disc_image.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The CHD writer follows the CHD v5 layout that libchdr reads (MAME's format, BSD-3-Clause): a
// 124-byte header, the DVD metadata chdman writes, the compressed hunks, then the compressed map.
// Each piece was checked by reading the images back with libchdr (ps5/tests/discimage).

#include "ps5_disc_image.h"

#include <LzmaEnc.h>
#include <zlib.h>
#include <zstd.h>

#include <Alloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace PS5DiscImage
{
	namespace
	{
		constexpr std::uint32_t SECTOR = 2048;

		void PutBE(std::uint8_t* out, std::uint64_t value, int bytes)
		{
			for (int i = bytes - 1; i >= 0; i--)
			{
				out[i] = static_cast<std::uint8_t>(value);
				value >>= 8;
			}
		}

		void PutLE(std::uint8_t* out, std::uint64_t value, int bytes)
		{
			for (int i = 0; i < bytes; i++)
			{
				out[i] = static_cast<std::uint8_t>(value);
				value >>= 8;
			}
		}

		constexpr std::uint32_t Tag(char a, char b, char c, char d)
		{
			return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(a)) << 24) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b)) << 16) |
				   (static_cast<std::uint32_t>(static_cast<std::uint8_t>(c)) << 8) | static_cast<std::uint32_t>(static_cast<std::uint8_t>(d));
		}

		// ---- a few workers that each compress a share of a batch

		class Workers
		{
		public:
			explicit Workers(unsigned count)
			{
				for (unsigned i = 0; i < count; i++)
					m_threads.emplace_back([this, i] { Run(i); });
			}
			~Workers()
			{
				{
					const std::lock_guard lock(m_mutex);
					m_quit = true;
				}
				m_wake.notify_all();
				for (std::thread& t : m_threads)
					t.join();
			}
			unsigned Count() const { return static_cast<unsigned>(m_threads.size()); }

			/// job(worker, item) for every item in [0, items), on the workers; returns at once.
			void Start(std::size_t items, std::function<void(unsigned, std::size_t)> job)
			{
				{
					const std::lock_guard lock(m_mutex);
					m_job = std::move(job);
					m_items = items;
					m_next.store(0);
					m_left = static_cast<unsigned>(m_threads.size());
					m_round++;
				}
				m_wake.notify_all();
			}
			/// Waits for the batch Start gave them.
			void Wait()
			{
				std::unique_lock lock(m_mutex);
				m_done.wait(lock, [this] { return m_left == 0; });
			}

		private:
			void Run(unsigned index)
			{
				std::uint64_t seen = 0;
				for (;;)
				{
					std::function<void(unsigned, std::size_t)> job;
					std::size_t items;
					{
						std::unique_lock lock(m_mutex);
						m_wake.wait(lock, [&] { return m_quit || m_round != seen; });
						if (m_quit)
							return;
						seen = m_round;
						job = m_job;
						items = m_items;
					}
					for (std::size_t item = m_next.fetch_add(1); item < items; item = m_next.fetch_add(1))
						job(index, item);
					{
						const std::lock_guard lock(m_mutex);
						m_left--;
					}
					m_done.notify_all();
				}
			}

			std::vector<std::thread> m_threads;
			std::mutex m_mutex;
			std::condition_variable m_wake, m_done;
			std::function<void(unsigned, std::size_t)> m_job;
			std::size_t m_items = 0;
			std::atomic<std::size_t> m_next{0};
			unsigned m_left = 0;
			std::uint64_t m_round = 0;
			bool m_quit = false;
		};

		// ---- ISO: the sectors as they are

		class IsoEncoder final : public Encoder
		{
		public:
			explicit IsoEncoder(Append append) : m_append(std::move(append)) {}
			bool Add(const std::uint8_t* data, std::size_t size) override
			{
				if (!m_append(data, size))
				{
					m_error = "The image could not be written.";
					return false;
				}
				m_written += size;
				return true;
			}
			bool Finish(std::vector<Patch>* patches) override
			{
				patches->clear();
				return true;
			}
			std::uint64_t Written() const override { return m_written; }
			std::string Error() const override { return m_error; }

		private:
			Append m_append;
			std::uint64_t m_written = 0;
			std::string m_error;
		};

		// ---- the compressed formats: blocks of the disc, compressed a batch at a time while the
		// next batch is read, written in order.

		class BlockEncoder : public Encoder
		{
		public:
			BlockEncoder(std::uint64_t total, std::uint32_t block, unsigned threads, Append append)
				: m_total(total)
				, m_block(block)
				, m_append(std::move(append))
				, m_workers(std::max(1u, threads))
			{
				m_batchBlocks = std::max<std::size_t>(8, static_cast<std::size_t>(m_workers.Count()) * 16);
				for (Batch& b : m_batches)
				{
					b.input.resize(m_batchBlocks * m_block);
					b.output.resize(m_batchBlocks);
				}
				m_blockCount = static_cast<std::size_t>((total + block - 1) / block);
			}
			~BlockEncoder() override
			{
				if (m_busy)
					m_workers.Wait();
			}

			bool Add(const std::uint8_t* data, std::size_t size) override
			{
				while (size > 0 && m_error.empty())
				{
					Batch& b = m_batches[m_filling];
					const std::size_t room = b.input.size() - b.used;
					const std::size_t take = std::min(room, size);
					std::memcpy(b.input.data() + b.used, data, take);
					b.used += take;
					data += take;
					size -= take;
					m_fed += take;
					if (b.used == b.input.size())
						Launch();
				}
				return m_error.empty();
			}

			bool Finish(std::vector<Patch>* patches) override
			{
				if (!m_error.empty())
					return false;
				if (m_fed != m_total)
				{
					m_error = "The disc ended before its size.";
					return false;
				}
				if (m_batches[m_filling].used > 0)
					Launch();
				Drain();
				if (!m_error.empty())
					return false;
				return Close(patches);
			}

			std::uint64_t Written() const override { return m_written; }
			std::string Error() const override { return m_error; }

		protected:
			struct Compressed
			{
				std::vector<std::uint8_t> bytes; // empty: stored as it is
				int codec = 0;
				std::uint16_t crc = 0;
			};

			/// One block (always m_block bytes, the last padded with zeros) compressed by `worker`.
			virtual void Compress(unsigned worker, const std::uint8_t* block, Compressed* out) = 0;
			/// A block's bytes go into the file: the encoder notes where and how.
			virtual void Placed(std::size_t index, const std::uint8_t* raw, const Compressed& c, std::uint64_t offset, std::uint32_t length) = 0;
			/// What comes after the last block, and the patches.
			virtual bool Close(std::vector<Patch>* patches) = 0;
			/// Writes what is before the first block.
			bool Emit(const void* data, std::size_t size)
			{
				if (!m_error.empty())
					return false;
				if (!m_append(data, size))
				{
					m_error = "The image could not be written (is the drive full?).";
					return false;
				}
				m_written += size;
				return true;
			}
			virtual std::uint32_t Padding(std::uint64_t /*offset*/) const { return 0; }

			std::uint64_t m_total;
			std::uint32_t m_block;
			std::size_t m_blockCount = 0;
			std::string m_error;

		private:
			struct Batch
			{
				std::vector<std::uint8_t> input;
				std::vector<Compressed> output;
				std::size_t used = 0;
				std::size_t first = 0; // index of its first block
				std::size_t blocks = 0;
			};

			// The batch being filled goes to the workers; the one they had before is written first.
			void Launch()
			{
				Drain();
				if (!m_error.empty())
					return;
				Batch& b = m_batches[m_filling];
				if (b.used % m_block != 0)
					std::memset(b.input.data() + b.used, 0, m_block - b.used % m_block);
				b.blocks = (b.used + m_block - 1) / m_block;
				b.first = m_nextBlock;
				m_nextBlock += b.blocks;
				Batch* const batch = &b;
				m_workers.Start(b.blocks, [this, batch](unsigned worker, std::size_t i) {
					Compress(worker, batch->input.data() + i * m_block, &batch->output[i]);
				});
				m_busy = true;
				m_pending = m_filling;
				m_filling ^= 1;
				m_batches[m_filling].used = 0;
			}

			// The batch the workers have, once done, into the file.
			void Drain()
			{
				if (!m_busy)
					return;
				m_workers.Wait();
				m_busy = false;
				Batch& b = m_batches[m_pending];
				for (std::size_t i = 0; i < b.blocks && m_error.empty(); i++)
				{
					const Compressed& c = b.output[i];
					const std::uint8_t* const raw = b.input.data() + i * m_block;
					const std::uint32_t pad = Padding(m_written);
					if (pad > 0)
					{
						static const std::uint8_t zeros[64] = {};
						Emit(zeros, pad);
					}
					const std::uint64_t at = m_written;
					const bool stored = c.bytes.empty();
					const std::uint32_t length = stored ? m_block : static_cast<std::uint32_t>(c.bytes.size());
					Placed(b.first + i, raw, c, at, length);
					Emit(stored ? raw : c.bytes.data(), length);
				}
				b.used = 0;
			}

			Append m_append;
			Workers m_workers;
			Batch m_batches[2];
			std::size_t m_batchBlocks = 0;
			int m_filling = 0, m_pending = 0;
			bool m_busy = false;
			std::size_t m_nextBlock = 0;
			std::uint64_t m_fed = 0;
			std::uint64_t m_written = 0;
		};

		// ---- CHD v5

		constexpr std::uint32_t V5_HEADER = 124;
		constexpr std::uint32_t CHD_HUNK = 16384; // eight sectors
		constexpr std::uint8_t COMPRESSION_TYPE_0 = 0, COMPRESSION_NONE = 4, COMPRESSION_RLE_SMALL = 7, COMPRESSION_RLE_LARGE = 8;

		// What the bits of the map are written with: most significant first, as libchdr reads them.
		class BitWriter
		{
		public:
			void Write(std::uint32_t value, int bits)
			{
				for (int i = bits - 1; i >= 0; i--)
				{
					m_byte = static_cast<std::uint8_t>((m_byte << 1) | ((value >> i) & 1));
					if (++m_count == 8)
					{
						m_out.push_back(m_byte);
						m_byte = 0;
						m_count = 0;
					}
				}
			}
			std::vector<std::uint8_t> Finish()
			{
				if (m_count > 0)
					m_out.push_back(static_cast<std::uint8_t>(m_byte << (8 - m_count)));
				m_count = 0;
				m_byte = 0;
				return std::move(m_out);
			}

		private:
			std::vector<std::uint8_t> m_out;
			std::uint8_t m_byte = 0;
			int m_count = 0;
		};

		class ChdEncoder final : public BlockEncoder
		{
		public:
			ChdEncoder(std::uint64_t total, bool lzma, unsigned threads, Append append)
				: BlockEncoder(total, CHD_HUNK, threads, std::move(append))
				, m_lzma(lzma)
			{
				m_map.resize(m_blockCount * 12);
				m_zstd.resize(threads == 0 ? 1 : threads, nullptr);
				m_scratch.resize(m_zstd.size());
				for (std::vector<std::uint8_t>& s : m_scratch)
					s.resize(CHD_HUNK + CHD_HUNK / 2 + 1024);

				// The header with what is not known yet left as zeros, then the DVD's tag as chdman
				// writes it (an empty string, its checksum counted in the overall SHA-1).
				std::uint8_t header[V5_HEADER] = {};
				std::memcpy(header, "MComprHD", 8);
				PutBE(header + 8, V5_HEADER, 4);
				PutBE(header + 12, 5, 4);
				PutBE(header + 16, m_lzma ? Tag('l', 'z', 'm', 'a') : Tag('z', 's', 't', 'd'), 4);
				PutBE(header + 32, total, 8);
				PutBE(header + 48, V5_HEADER, 8); // the metadata follows the header
				PutBE(header + 56, CHD_HUNK, 4);
				PutBE(header + 60, SECTOR, 4);
				m_header.assign(header, header + V5_HEADER);
				Emit(header, sizeof(header));
				std::uint8_t meta[16 + 1] = {};
				PutBE(meta, Tag('D', 'V', 'D', ' '), 4);
				meta[4] = 0x01; // CHD_MDFLAGS_CHECKSUM
				PutBE(meta + 5, 1, 3);
				// (next: 0, the last entry; then the data, one NUL)
				Emit(meta, sizeof(meta));
			}
			~ChdEncoder() override
			{
				for (ZSTD_CCtx* c : m_zstd)
					ZSTD_freeCCtx(c);
			}

		protected:
			void Compress(unsigned worker, const std::uint8_t* block, Compressed* out) override
			{
				out->crc = Crc16(block, CHD_HUNK);
				out->bytes.clear();
				std::vector<std::uint8_t>& dest = m_scratch[worker];
				std::size_t length = 0;
				if (m_lzma)
				{
					// The properties libchdr's decoder takes for granted: level 9, sized to the hunk.
					CLzmaEncProps props;
					LzmaEncProps_Init(&props);
					props.level = 9;
					props.reduceSize = CHD_HUNK;
					LzmaEncProps_Normalize(&props);
					CLzmaEncHandle enc = LzmaEnc_Create(&g_Alloc);
					if (enc == nullptr)
						return;
					SizeT destLen = dest.size();
					if (LzmaEnc_SetProps(enc, &props) == SZ_OK &&
						LzmaEnc_MemEncode(enc, dest.data(), &destLen, block, CHD_HUNK, 0, nullptr, &g_Alloc, &g_Alloc) == SZ_OK)
						length = destLen;
					LzmaEnc_Destroy(enc, &g_Alloc, &g_Alloc);
				}
				else
				{
					ZSTD_CCtx*& ctx = m_zstd[worker];
					if (ctx == nullptr)
						ctx = ZSTD_createCCtx();
					if (ctx != nullptr)
					{
						const std::size_t r = ZSTD_compressCCtx(ctx, dest.data(), dest.size(), block, CHD_HUNK, 12);
						if (!ZSTD_isError(r))
							length = r;
					}
				}
				// Kept only when it is smaller: otherwise the hunk is stored as it is.
				if (length > 0 && length < CHD_HUNK)
					out->bytes.assign(dest.begin(), dest.begin() + static_cast<std::ptrdiff_t>(length));
			}

			void Placed(std::size_t index, const std::uint8_t* raw, const Compressed& c, std::uint64_t offset, std::uint32_t length) override
			{
				// The raw SHA-1 is of the disc's own bytes: not of the last hunk's padding.
				const std::uint64_t start = static_cast<std::uint64_t>(index) * CHD_HUNK;
				const std::uint64_t real = std::min<std::uint64_t>(CHD_HUNK, m_total - start);
				m_rawSha.Add(raw, static_cast<std::size_t>(real));
				std::uint8_t* const e = m_map.data() + index * 12;
				e[0] = c.bytes.empty() ? COMPRESSION_NONE : COMPRESSION_TYPE_0;
				PutBE(e + 1, length, 3);
				PutBE(e + 4, offset, 6);
				PutBE(e + 10, c.crc, 2);
			}

			bool Close(std::vector<Patch>* patches) override
			{
				// The map, compressed as libchdr reads it: the kinds of the hunks, run-length coded
				// and Huffman coded (sixteen 4-bit codes: symbol n is code n), then each hunk's length
				// and CRC.
				const std::uint32_t hunks = static_cast<std::uint32_t>(m_blockCount);
				const std::uint16_t mapCrc = Crc16(m_map.data(), m_map.size());
				std::vector<std::uint8_t> rle;
				rle.reserve(hunks);
				std::uint32_t maxLength = 0;
				std::uint8_t last = 0;
				int count = 0;
				for (std::uint32_t h = 0; h < hunks; h++)
				{
					const std::uint8_t kind = m_map[h * 12];
					maxLength = std::max<std::uint32_t>(maxLength, (m_map[h * 12 + 1] << 16) | (m_map[h * 12 + 2] << 8) | m_map[h * 12 + 3]);
					if (kind == last)
						count++;
					if (kind != last || h == hunks - 1)
					{
						while (count != 0)
						{
							if (count < 3)
							{
								rle.push_back(last);
								count--;
							}
							else if (count <= 3 + 15)
							{
								rle.push_back(COMPRESSION_RLE_SMALL);
								rle.push_back(static_cast<std::uint8_t>(count - 3));
								count = 0;
							}
							else
							{
								const int take = std::min(count, 3 + 16 + 255);
								rle.push_back(COMPRESSION_RLE_LARGE);
								rle.push_back(static_cast<std::uint8_t>((take - 3 - 16) >> 4));
								rle.push_back(static_cast<std::uint8_t>((take - 3 - 16) & 15));
								count -= take;
							}
						}
						if (kind != last)
						{
							rle.push_back(kind);
							last = kind;
						}
					}
				}
				int lengthBits = 0;
				while ((maxLength >> lengthBits) != 0)
					lengthBits++;

				BitWriter bits;
				for (int code = 0; code < 16; code++)
					bits.Write(4, 4); // every code 4 bits long (4 is not the escape value 1)
				for (std::uint8_t symbol : rle)
					bits.Write(symbol, 4);
				std::uint64_t firstOffset = 0;
				for (std::uint32_t h = 0; h < hunks; h++)
				{
					const std::uint8_t* const e = m_map.data() + h * 12;
					const std::uint32_t length = (e[1] << 16) | (e[2] << 8) | e[3];
					const std::uint16_t crc = static_cast<std::uint16_t>((e[10] << 8) | e[11]);
					if (h == 0)
					{
						for (int i = 4; i < 10; i++)
							firstOffset = (firstOffset << 8) | e[i];
					}
					if (e[0] == COMPRESSION_TYPE_0)
						bits.Write(length, lengthBits);
					bits.Write(crc, 16);
				}
				const std::vector<std::uint8_t> packed = bits.Finish();
				std::uint8_t mapHeader[16] = {};
				PutBE(mapHeader, packed.size(), 4);
				PutBE(mapHeader + 4, firstOffset, 6);
				PutBE(mapHeader + 10, mapCrc, 2);
				mapHeader[12] = static_cast<std::uint8_t>(lengthBits);
				const std::uint64_t mapOffset = Written();
				if (!Emit(mapHeader, sizeof(mapHeader)) || !Emit(packed.data(), packed.size()))
					return false;

				// The two SHA-1s: of the disc, and of that with the metadata's (tag + SHA-1 of the
				// one NUL byte).
				std::uint8_t raw[20], overall[20];
				m_rawSha.Finish(raw);
				std::uint8_t metaHash[24];
				PutBE(metaHash, Tag('D', 'V', 'D', ' '), 4);
				{
					Sha1 s;
					const std::uint8_t nul = 0;
					s.Add(&nul, 1);
					s.Finish(metaHash + 4);
				}
				{
					Sha1 s;
					s.Add(raw, 20);
					s.Add(metaHash, sizeof(metaHash));
					s.Finish(overall);
				}
				PutBE(m_header.data() + 40, mapOffset, 8);
				std::memcpy(m_header.data() + 64, raw, 20);
				std::memcpy(m_header.data() + 84, overall, 20);
				patches->clear();
				patches->push_back(Patch{0, m_header});
				return true;
			}

		private:
			bool m_lzma;
			std::vector<std::uint8_t> m_map;
			std::vector<std::uint8_t> m_header;
			std::vector<ZSTD_CCtx*> m_zstd;
			std::vector<std::vector<std::uint8_t>> m_scratch;
			Sha1 m_rawSha;
		};

		// ---- CSO v1

		constexpr std::uint32_t CSO_FRAME = 2048;

		class CsoEncoder final : public BlockEncoder
		{
		public:
			CsoEncoder(std::uint64_t total, unsigned threads, Append append)
				: BlockEncoder(total, CSO_FRAME, threads, std::move(append))
			{
				// The index holds offsets >> align in 31 bits: align is as small as the image's
				// largest offset allows (it is never larger than the disc and its index).
				const std::uint64_t most = total + (m_blockCount + 1) * 4 + 64;
				while ((most >> m_align) >= 0x80000000ull)
					m_align++;
				m_index.resize(m_blockCount + 1);
				m_scratch.resize(std::max(1u, threads));
				for (std::vector<std::uint8_t>& s : m_scratch)
					s.resize(compressBound(CSO_FRAME) + 64);
				std::uint8_t header[24] = {};
				std::memcpy(header, "CISO", 4);
				PutLE(header + 4, 24, 4);
				PutLE(header + 8, total, 8);
				PutLE(header + 16, CSO_FRAME, 4);
				header[20] = 1;
				header[21] = static_cast<std::uint8_t>(m_align);
				m_header.assign(header, header + 24);
				Emit(header, sizeof(header));
				// The index's place, written over at the end.
				const std::vector<std::uint8_t> zeros(m_index.size() * 4, 0);
				Emit(zeros.data(), zeros.size());
			}

		protected:
			void Compress(unsigned worker, const std::uint8_t* block, Compressed* out) override
			{
				out->bytes.clear();
				std::vector<std::uint8_t>& dest = m_scratch[worker];
				z_stream z = {};
				if (deflateInit2(&z, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY) != Z_OK)
					return;
				z.next_in = const_cast<Bytef*>(block);
				z.avail_in = CSO_FRAME;
				z.next_out = dest.data();
				z.avail_out = static_cast<uInt>(dest.size());
				const int r = deflate(&z, Z_FINISH);
				const std::size_t length = dest.size() - z.avail_out;
				deflateEnd(&z);
				// Kept when it is smaller even once padded to the alignment; else stored.
				const std::size_t unit = std::size_t(1) << m_align;
				if (r == Z_STREAM_END && length + unit < CSO_FRAME)
					out->bytes.assign(dest.begin(), dest.begin() + static_cast<std::ptrdiff_t>(length));
			}

			std::uint32_t Padding(std::uint64_t offset) const override
			{
				const std::uint64_t unit = std::uint64_t(1) << m_align;
				return static_cast<std::uint32_t>((unit - (offset & (unit - 1))) & (unit - 1));
			}

			void Placed(std::size_t index, const std::uint8_t*, const Compressed& c, std::uint64_t offset, std::uint32_t) override
			{
				m_index[index] = static_cast<std::uint32_t>(offset >> m_align) | (c.bytes.empty() ? 0x80000000u : 0u);
			}

			bool Close(std::vector<Patch>* patches) override
			{
				// The end of the last frame, aligned.
				const std::uint32_t pad = Padding(Written());
				if (pad > 0)
				{
					static const std::uint8_t zeros[64] = {};
					if (!Emit(zeros, pad))
						return false;
				}
				m_index[m_blockCount] = static_cast<std::uint32_t>(Written() >> m_align);
				Patch p;
				p.offset = 24;
				p.bytes.resize(m_index.size() * 4);
				for (std::size_t i = 0; i < m_index.size(); i++)
					PutLE(p.bytes.data() + i * 4, m_index[i], 4);
				patches->clear();
				patches->push_back(std::move(p));
				return true;
			}

		private:
			int m_align = 0;
			std::vector<std::uint32_t> m_index;
			std::vector<std::uint8_t> m_header;
			std::vector<std::vector<std::uint8_t>> m_scratch;
		};
	} // namespace

	const char* Extension(Format format)
	{
		switch (format)
		{
			case Format::ChdSmall:
			case Format::ChdFast:
				return ".chd";
			case Format::Cso:
				return ".cso";
			default:
				return ".iso";
		}
	}

	const char* Name(Format format)
	{
		switch (format)
		{
			case Format::ChdSmall:
				return "CHD, smallest";
			case Format::ChdFast:
				return "CHD, quickest to load";
			case Format::Cso:
				return "CSO";
			default:
				return "ISO";
		}
	}

	const char* Description(Format format)
	{
		switch (format)
		{
			case Format::ChdSmall:
				return "Compressed with LZMA: often half the disc's size or less. Slowest to make";
			case Format::ChdFast:
				return "Compressed with Zstandard: a little larger, quick to make and to read";
			case Format::Cso:
				return "Compressed in small pieces with deflate: smaller than an ISO, larger than a CHD";
			default:
				return "Every sector as it is, as large as the disc: read by everything, the quickest to make";
		}
	}

	std::unique_ptr<Encoder> Encoder::Create(Format format, std::uint64_t total, unsigned threads, Append append)
	{
		switch (format)
		{
			case Format::ChdSmall:
				return std::make_unique<ChdEncoder>(total, true, threads, std::move(append));
			case Format::ChdFast:
				return std::make_unique<ChdEncoder>(total, false, threads, std::move(append));
			case Format::Cso:
				return std::make_unique<CsoEncoder>(total, threads, std::move(append));
			default:
				return std::make_unique<IsoEncoder>(std::move(append));
		}
	}

	// ---- CRC-16 (CCITT, from 0xffff: what the CHD map holds) and SHA-1

	std::uint16_t Crc16(const void* data, std::size_t size)
	{
		static const auto table = [] {
			std::array<std::uint16_t, 256> t{};
			for (unsigned i = 0; i < 256; i++)
			{
				std::uint16_t c = static_cast<std::uint16_t>(i << 8);
				for (int b = 0; b < 8; b++)
					c = static_cast<std::uint16_t>((c & 0x8000) ? (c << 1) ^ 0x1021 : (c << 1));
				t[i] = c;
			}
			return t;
		}();
		std::uint16_t crc = 0xffff;
		const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
		for (std::size_t i = 0; i < size; i++)
			crc = static_cast<std::uint16_t>((crc << 8) ^ table[((crc >> 8) ^ p[i]) & 0xff]);
		return crc;
	}

	Sha1::Sha1()
	{
		m_h[0] = 0x67452301;
		m_h[1] = 0xEFCDAB89;
		m_h[2] = 0x98BADCFE;
		m_h[3] = 0x10325476;
		m_h[4] = 0xC3D2E1F0;
	}

	void Sha1::Block(const std::uint8_t* p)
	{
		const auto rol = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
		std::uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) | (std::uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
		for (int i = 16; i < 80; i++)
			w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		std::uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4];
		for (int i = 0; i < 80; i++)
		{
			std::uint32_t f, k;
			if (i < 20)
				f = (b & c) | (~b & d), k = 0x5A827999;
			else if (i < 40)
				f = b ^ c ^ d, k = 0x6ED9EBA1;
			else if (i < 60)
				f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
			else
				f = b ^ c ^ d, k = 0xCA62C1D6;
			const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = rol(b, 30);
			b = a;
			a = t;
		}
		m_h[0] += a;
		m_h[1] += b;
		m_h[2] += c;
		m_h[3] += d;
		m_h[4] += e;
	}

	void Sha1::Add(const void* data, std::size_t size)
	{
		const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
		m_bytes += size;
		if (m_used > 0)
		{
			const std::size_t take = std::min(size, 64 - m_used);
			std::memcpy(m_buffer + m_used, p, take);
			m_used += take;
			p += take;
			size -= take;
			if (m_used == 64)
			{
				Block(m_buffer);
				m_used = 0;
			}
		}
		while (size >= 64)
		{
			Block(p);
			p += 64;
			size -= 64;
		}
		if (size > 0)
		{
			std::memcpy(m_buffer, p, size);
			m_used = size;
		}
	}

	void Sha1::Finish(std::uint8_t out[20])
	{
		const std::uint64_t bits = m_bytes * 8;
		const std::uint8_t one = 0x80;
		Add(&one, 1);
		const std::uint8_t zero = 0;
		while (m_used != 56)
			Add(&zero, 1);
		std::uint8_t length[8];
		PutBE(length, bits, 8);
		Add(length, 8);
		for (int i = 0; i < 5; i++)
			PutBE(out + i * 4, m_h[i], 4);
	}
} // namespace PS5DiscImage
