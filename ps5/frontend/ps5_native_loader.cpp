// Crux2 - a loader for a game's native module (ps5_native_loader.h).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_native_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <elf.h>

#ifndef DT_GNU_HASH
#define DT_GNU_HASH 0x6ffffef5
#endif

#ifdef __PROSPERO__
#include <ps5platform/exec.h>
#else
#include <sys/mman.h>
#endif

namespace PS5NativeLoader
{
	namespace
	{
		constexpr size_t kMaxImage = 512u * 1024u * 1024u;

		void* AllocateImage(size_t bytes)
		{
#ifdef __PROSPERO__
			// Read, write and execute at once (the platform layer's default region).
			return ps5_exec_allocate(bytes, 0);
#else
			void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			return (p == MAP_FAILED) ? nullptr : p;
#endif
		}

		void FreeImage(void* base, size_t bytes)
		{
#ifdef __PROSPERO__
			(void)bytes;
			ps5_exec_release(base);
#else
			munmap(base, bytes);
#endif
		}

		class LoadedModule final : public Module
		{
		public:
			~LoadedModule() override
			{
				if (m_base)
					FreeImage(m_base, m_bytes);
			}

			void* Symbol(const char* name) const override
			{
				if (!m_symtab || !m_strtab)
					return nullptr;
				for (uint32_t i = 1; i < m_symbols; i++)
				{
					const Elf64_Sym& s = m_symtab[i];
					if (s.st_shndx == SHN_UNDEF || s.st_name >= m_strsz)
						continue;
					if (std::strcmp(m_strtab + s.st_name, name) == 0)
						return m_base + s.st_value;
				}
				return nullptr;
			}

			size_t Size() const override { return m_bytes; }

			uint8_t* m_base = nullptr;
			size_t m_bytes = 0;
			const Elf64_Sym* m_symtab = nullptr;
			uint32_t m_symbols = 0;
			const char* m_strtab = nullptr;
			size_t m_strsz = 0;
		};

		bool Fail(std::string* error, const char* what)
		{
			if (error)
				*error = what;
			return false;
		}
	} // namespace

	std::unique_ptr<Module> Load(const uint8_t* data, size_t size, std::string* error)
	{
		if (size < sizeof(Elf64_Ehdr))
			return Fail(error, "too small to be a module"), nullptr;
		Elf64_Ehdr eh;
		std::memcpy(&eh, data, sizeof(eh));
		if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB ||
			eh.e_machine != EM_X86_64 || eh.e_type != ET_DYN)
			return Fail(error, "not an x86-64 shared object"), nullptr;
		if (eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phoff > size || eh.e_phnum > (size - eh.e_phoff) / sizeof(Elf64_Phdr))
			return Fail(error, "its program headers are damaged"), nullptr;

		std::vector<Elf64_Phdr> ph(eh.e_phnum);
		std::memcpy(ph.data(), data + eh.e_phoff, ph.size() * sizeof(Elf64_Phdr));

		// The span of the loaded segments.
		uint64_t lo = UINT64_MAX, hi = 0;
		const Elf64_Phdr* dynamic = nullptr;
		for (const Elf64_Phdr& p : ph)
		{
			if (p.p_type == PT_LOAD)
			{
				if (p.p_offset > size || p.p_filesz > size - p.p_offset || p.p_filesz > p.p_memsz)
					return Fail(error, "a segment lies outside the file"), nullptr;
				lo = std::min<uint64_t>(lo, p.p_vaddr & ~uint64_t{0xFFF});
				hi = std::max<uint64_t>(hi, p.p_vaddr + p.p_memsz);
			}
			else if (p.p_type == PT_DYNAMIC)
			{
				dynamic = &p;
			}
		}
		if (lo != 0 || hi == 0 || hi > kMaxImage)
			return Fail(error, "its segments are not laid out from 0 or are too large"), nullptr;

		auto module = std::make_unique<LoadedModule>();
		module->m_bytes = (hi + 0x3FFF) & ~uint64_t{0x3FFF};
		module->m_base = static_cast<uint8_t*>(AllocateImage(module->m_bytes));
		if (!module->m_base)
			return Fail(error, "no executable memory for it"), nullptr;
		std::memset(module->m_base, 0, module->m_bytes);
		for (const Elf64_Phdr& p : ph)
		{
			if (p.p_type == PT_LOAD)
				std::memcpy(module->m_base + p.p_vaddr, data + p.p_offset, p.p_filesz);
		}
		if (!dynamic)
			return Fail(error, "it has no dynamic section"), nullptr;

		// The dynamic section: relocations, symbols, initialisers.
		uint64_t rela = 0, relasz = 0, symtab = 0, strtab = 0, strsz = 0, hash = 0, gnu_hash = 0, init_array = 0, init_arraysz = 0, init = 0;
		bool imports = false;
		for (const Elf64_Dyn* d = reinterpret_cast<const Elf64_Dyn*>(module->m_base + dynamic->p_vaddr); d->d_tag != DT_NULL; d++)
		{
			switch (d->d_tag)
			{
				case DT_RELA: rela = d->d_un.d_ptr; break;
				case DT_RELASZ: relasz = d->d_un.d_val; break;
				case DT_SYMTAB: symtab = d->d_un.d_ptr; break;
				case DT_STRTAB: strtab = d->d_un.d_ptr; break;
				case DT_STRSZ: strsz = d->d_un.d_val; break;
				case DT_HASH: hash = d->d_un.d_ptr; break;
				case DT_GNU_HASH: gnu_hash = d->d_un.d_ptr; break;
				case DT_INIT_ARRAY: init_array = d->d_un.d_ptr; break;
				case DT_INIT_ARRAYSZ: init_arraysz = d->d_un.d_val; break;
				case DT_INIT: init = d->d_un.d_ptr; break;
				case DT_NEEDED: imports = true; break;
				case DT_JMPREL:
				case DT_PLTRELSZ:
					imports = true;
					break;
				default: break;
			}
		}
		if (imports)
			return Fail(error, "it needs other libraries or has imports (a module must be built by build-module.py)"), nullptr;
		if (rela + relasz > hi || symtab >= hi || strtab + strsz > hi || hash >= hi || gnu_hash >= hi || init_array + init_arraysz > hi)
			return Fail(error, "its dynamic section points outside it"), nullptr;

		for (uint64_t off = 0; off + sizeof(Elf64_Rela) <= relasz; off += sizeof(Elf64_Rela))
		{
			Elf64_Rela r;
			std::memcpy(&r, module->m_base + rela + off, sizeof(r));
			if (ELF64_R_TYPE(r.r_info) == R_X86_64_NONE)
				continue;
			if (ELF64_R_TYPE(r.r_info) != R_X86_64_RELATIVE)
				return Fail(error, "it has a relocation other than a relative one"), nullptr;
			if (r.r_offset + sizeof(uint64_t) > hi)
				return Fail(error, "a relocation points outside it"), nullptr;
			const uint64_t value = reinterpret_cast<uint64_t>(module->m_base) + static_cast<uint64_t>(r.r_addend);
			std::memcpy(module->m_base + r.r_offset, &value, sizeof(value));
		}

		module->m_symtab = reinterpret_cast<const Elf64_Sym*>(module->m_base + symtab);
		module->m_strtab = reinterpret_cast<const char*>(module->m_base + strtab);
		module->m_strsz = strsz;
		// The number of dynamic symbols: from the section headers when the file has them (.dynsym's
		// size), else from DT_HASH (its second word), else by walking DT_GNU_HASH's chains. A
		// linker that writes only the GNU table (some distributions' default) leaves no DT_HASH.
		uint32_t count = 0;
		if (eh.e_shoff && eh.e_shentsize == sizeof(Elf64_Shdr) && eh.e_shoff <= size &&
			eh.e_shnum <= (size - eh.e_shoff) / sizeof(Elf64_Shdr))
		{
			for (uint16_t i = 0; i < eh.e_shnum; i++)
			{
				Elf64_Shdr sh;
				std::memcpy(&sh, data + eh.e_shoff + i * sizeof(Elf64_Shdr), sizeof(sh));
				if (sh.sh_type == SHT_DYNSYM && sh.sh_addr == symtab && sh.sh_entsize == sizeof(Elf64_Sym))
					count = static_cast<uint32_t>(sh.sh_size / sizeof(Elf64_Sym));
			}
		}
		if (!count && hash)
			std::memcpy(&count, module->m_base + hash + 4, sizeof(uint32_t));
		if (!count && gnu_hash && gnu_hash + 16 <= hi)
		{
			uint32_t nbuckets, symoffset, bloom_size;
			std::memcpy(&nbuckets, module->m_base + gnu_hash, 4);
			std::memcpy(&symoffset, module->m_base + gnu_hash + 4, 4);
			std::memcpy(&bloom_size, module->m_base + gnu_hash + 8, 4);
			const uint64_t buckets = gnu_hash + 16 + uint64_t{bloom_size} * 8;
			const uint64_t chains = buckets + uint64_t{nbuckets} * 4;
			uint32_t last = 0;
			for (uint32_t b = 0; b < nbuckets && buckets + (b + 1) * 4ull <= hi; b++)
			{
				uint32_t v;
				std::memcpy(&v, module->m_base + buckets + b * 4ull, 4);
				last = std::max(last, v);
			}
			if (last >= symoffset)
			{
				// The last bucket's chain ends at the entry with its low bit set.
				for (;;)
				{
					const uint64_t at = chains + uint64_t{last - symoffset} * 4;
					if (at + 4 > hi)
						break;
					uint32_t h;
					std::memcpy(&h, module->m_base + at, 4);
					last++;
					if (h & 1)
						break;
				}
			}
			count = std::max(last, symoffset);
		}
		if (symtab + uint64_t{count} * sizeof(Elf64_Sym) > hi)
			return Fail(error, "its symbol table runs outside it"), nullptr;
		module->m_symbols = count;
		for (uint32_t i = 1; i < module->m_symbols; i++)
		{
			if (module->m_symtab[i].st_shndx == SHN_UNDEF && module->m_symtab[i].st_name != 0)
				return Fail(error, "it imports a symbol"), nullptr;
		}

		if (init)
			reinterpret_cast<void (*)()>(module->m_base + init)();
		for (uint64_t off = 0; off + sizeof(uint64_t) <= init_arraysz; off += sizeof(uint64_t))
		{
			uint64_t fn;
			std::memcpy(&fn, module->m_base + init_array + off, sizeof(fn));
			if (fn != 0 && fn != UINT64_MAX)
				reinterpret_cast<void (*)()>(fn)();
		}
		return module;
	}

	std::unique_ptr<Module> LoadFile(const std::string& path, std::string* error)
	{
		std::FILE* f = std::fopen(path.c_str(), "rb");
		if (!f)
			return Fail(error, "the file could not be opened"), nullptr;
		std::vector<uint8_t> data;
		uint8_t buffer[1 << 16];
		size_t got;
		while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
		{
			data.insert(data.end(), buffer, buffer + got);
			if (data.size() > kMaxImage)
				break;
		}
		std::fclose(f);
		return Load(data.data(), data.size(), error);
	}
} // namespace PS5NativeLoader
