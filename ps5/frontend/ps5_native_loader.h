// Crux2 - a loader for a game's native module (ps5/recomp): a freestanding x86-64 shared object
// with no imports, placed in executable memory with its relocations applied.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
//
// The console has no dlopen for a title, so the module is loaded here: its PT_LOAD segments into
// one executable region (the platform layer's ps5platform/exec.h on the console, anonymous memory
// on the PC), its R_X86_64_RELATIVE relocations applied (the only kind a module built by
// build-module.py has: anything else, or an import, is refused), its initialisers run, and its
// exported functions found by name. The shape is PS5 RetroArch's core loader
// (src/core_loader_ps5.cpp, Copyright (C) 2026 Mihawk, GPL-3.0-or-later), cut down to what a
// module with no imports needs.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace PS5NativeLoader
{
	class Module
	{
	public:
		virtual ~Module() = default;
		/// An exported function or object by name; nullptr when there is none.
		virtual void* Symbol(const char* name) const = 0;
		/// The bytes the module occupies.
		virtual size_t Size() const = 0;
	};

	/// Loads the module in `data` (the file's bytes). nullptr with `error` a sentence on failure.
	std::unique_ptr<Module> Load(const uint8_t* data, size_t size, std::string* error);

	/// Loads the module at `path`.
	std::unique_ptr<Module> LoadFile(const std::string& path, std::string* error);
} // namespace PS5NativeLoader
