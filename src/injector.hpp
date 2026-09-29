/*
 *  Injectors - Base Header (Linux port for eu4cjk)
 *
 *  Copyright (C) 2012-2014 LINK/2012 <dma_2012@hotmail.com> (original)
 *  Windows VirtualProtect -> mprotect + /proc/self/maps old-protection query
 *
 *  This software is provided 'as-is', without any express or implied
 *  warranty. In no event will the authors be held liable for any damages
 *  arising from the use of this software.
 *
 *  Permission is granted to anyone to use this software for any purpose,
 *  including commercial applications, and to alter it and redistribute it
 *  freely, subject to the following restrictions:
 *
 *     1. The origin of this software must not be misrepresented; you must not
 *     claim that you wrote the original software. If you use this software
 *     in a product, an acknowledgment in the product documentation would be
 *     appreciated but is not required.
 *
 *     2. Altered source versions must be plainly marked as such, and must not be
 *     misrepresented as being the original software.
 *
 *     3. This notice may not be removed or altered from any source
 *     distribution.
 *
 */
#pragma once
#define INJECTOR_HAS_INJECTOR_HPP

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

namespace Injector
{

	union memory_pointer_raw
	{
		void* p;
		uintptr_t a;

		memory_pointer_raw() : p(nullptr) {}
		memory_pointer_raw(const void* x) : p(const_cast<void*>(x)) {}
		memory_pointer_raw(uintptr_t x) : a(x) {}

		template<class T> T* get() const { return reinterpret_cast<T*>(p); }
		template<class T> T* get_raw() const { return reinterpret_cast<T*>(p); }
		void* get() const { return p; }

		memory_pointer_raw operator+(const memory_pointer_raw& rhs) const
		{
			return memory_pointer_raw(a + rhs.a);
		}

		memory_pointer_raw operator-(const memory_pointer_raw& rhs) const
		{
			return memory_pointer_raw(a - rhs.a);
		}

		bool operator==(const memory_pointer_raw& rhs) const { return a == rhs.a; }
		bool operator!=(const memory_pointer_raw& rhs) const { return a != rhs.a; }
		bool operator<(const memory_pointer_raw& rhs) const { return a < rhs.a; }
		bool operator<=(const memory_pointer_raw& rhs) const { return a <= rhs.a; }
		bool operator>(const memory_pointer_raw& rhs) const { return a > rhs.a; }
		bool operator>=(const memory_pointer_raw& rhs) const { return a >= rhs.a; }

		bool is_null() const { return p == nullptr; }
		uintptr_t as_int() const { return a; }

		explicit operator uintptr_t() const { return a; }
		explicit operator bool() const { return p != nullptr; }
	};

	typedef memory_pointer_raw memory_pointer_tr;

	namespace mem_protection
	{
		constexpr int NoAccess = PROT_NONE;
		constexpr int Read = PROT_READ;
		constexpr int Write = PROT_WRITE;
		constexpr int Execute = PROT_EXEC;
		constexpr int ReadWrite = PROT_READ | PROT_WRITE;
		constexpr int ReadExecute = PROT_READ | PROT_EXEC;
		constexpr int ReadWriteExecute = PROT_READ | PROT_WRITE | PROT_EXEC;
	}

	inline size_t page_size()
	{
		static size_t pg = static_cast<size_t>(sysconf(_SC_PAGESIZE));
		return pg;
	}

	inline memory_pointer_raw page_align(memory_pointer_tr addr)
	{
		return memory_pointer_raw(addr.a & ~(page_size() - 1));
	}

	inline bool QueryProtection(memory_pointer_tr addr, int& out_protection)
	{
		FILE* maps = fopen("/proc/self/maps", "r");
		if (!maps) return false;

		bool found = false;
		char line[1024];

		while (fgets(line, sizeof(line), maps))
		{
			unsigned long start = 0, end = 0;
			char perms[8] = { 0 };

			if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3)
				continue;

			if (addr.a >= start && addr.a < end)
			{
				int prot = PROT_NONE;
				if (perms[0] == 'r') prot |= PROT_READ;
				if (perms[1] == 'w') prot |= PROT_WRITE;
				if (perms[2] == 'x') prot |= PROT_EXEC;
				out_protection = prot;
				found = true;
				break;
			}
		}

		fclose(maps);
		return found;
	}

	inline bool ProtectMemory(memory_pointer_tr addr, size_t size, int protection)
	{
		memory_pointer_raw base = page_align(addr);
		size_t len = ((addr.a + size + page_size() - 1) & ~(page_size() - 1)) - base.a;
		return mprotect(base.get(), len, protection) == 0;
	}

	inline bool UnprotectMemory(memory_pointer_tr addr, size_t size, int& out_oldprotect)
	{
		if (!QueryProtection(addr, out_oldprotect))
			return false;

		int prot = PROT_READ | PROT_WRITE | (out_oldprotect & PROT_EXEC);
		return ProtectMemory(addr, size, prot);
	}

	struct scoped_unprotect
	{
		memory_pointer_raw  addr;
		size_t              size;
		int                 oldprotect;
		bool                bUnprotected;

		scoped_unprotect(memory_pointer_tr addr, size_t size)
		{
			if (size == 0) bUnprotected = false;
			else          bUnprotected = UnprotectMemory(this->addr = addr.a, this->size = size, oldprotect);
		}

		~scoped_unprotect()
		{
			if (bUnprotected) ProtectMemory(this->addr.get(), this->size, this->oldprotect);
		}
	};

	inline void WriteMemoryRaw(memory_pointer_tr addr, const void* value, size_t size, bool vp = true)
	{
		scoped_unprotect xprotect(addr, vp ? size : 0);
		memcpy(addr.get(), value, size);
	}

	inline void ReadMemoryRaw(memory_pointer_tr addr, void* ret, size_t size, bool vp = true)
	{
		scoped_unprotect xprotect(addr, vp ? size : 0);
		memcpy(ret, addr.get(), size);
	}

	inline void MemoryFill(memory_pointer_tr addr, uint8_t value, size_t size, bool vp = true)
	{
		scoped_unprotect xprotect(addr, vp ? size : 0);
		memset(addr.get(), value, size);
	}

	template<class T>
	inline T& WriteObject(memory_pointer_tr addr, const T& value, bool vp = true)
	{
		scoped_unprotect xprotect(addr, vp ? sizeof(value) : 0);
		memcpy(addr.get(), &value, sizeof(value));
		return *addr.get<T>();
	}

	template<class T>
	inline T& ReadObject(memory_pointer_tr addr, T& value, bool vp = true)
	{
		scoped_unprotect xprotect(addr, vp ? sizeof(value) : 0);
		memcpy(&value, addr.get(), sizeof(value));
		return value;
	}

	template<class T>
	inline memory_pointer_tr WriteMemory(memory_pointer_tr addr, T value, bool vp = true)
	{
		WriteObject(addr, value, vp);
		return addr + sizeof(value);
	}

	template<class T>
	inline T ReadMemory(memory_pointer_tr addr, bool vp = true)
	{
		T value;
		return ReadObject(addr, value, vp);
	}

	inline memory_pointer_raw GetRelativeOffset(memory_pointer_tr dest, memory_pointer_tr at)
	{
		return memory_pointer_raw(dest.a - at.a);
	}

	inline memory_pointer_raw MakeRelativeOffset(memory_pointer_tr at, memory_pointer_tr dest, size_t size = 4, bool vp = true)
	{
		int64_t rel = static_cast<int64_t>(static_cast<intptr_t>(dest.a) - static_cast<intptr_t>(at.a + size));

		if (size == 4)
			WriteMemory<int32_t>(at, static_cast<int32_t>(rel), vp);
		else if (size == 8)
			WriteMemory<int64_t>(at, rel, vp);
		else if (size == 2)
			WriteMemory<int16_t>(at, static_cast<int16_t>(rel), vp);
		else if (size == 1)
			WriteMemory<int8_t>(at, static_cast<int8_t>(rel), vp);

		return at + size;
	}

	inline memory_pointer_raw ReadRelativeOffset(memory_pointer_tr at, size_t size = 4, bool vp = true)
	{
		int64_t rel = 0;
		ReadMemoryRaw(at, &rel, size, vp);
		return memory_pointer_raw(static_cast<intptr_t>(at.a) + size + rel);
	}

	inline memory_pointer_raw GetAbsoluteOffset(int64_t rel_value, memory_pointer_tr end_of_instruction)
	{
		return memory_pointer_raw(static_cast<intptr_t>(end_of_instruction.a) + rel_value);
	}

	inline memory_pointer_raw GetBranchDestination(memory_pointer_tr at, bool vp = true)
	{
		switch (ReadMemory<uint8_t>(at, vp))
		{
		case 0x48:
		case 0x4C:
			switch (ReadMemory<uint8_t>(at + 1, vp))
			{
			case 0x8B:
			case 0x8D:
				switch (ReadMemory<uint8_t>(at + 2, vp))
				{
				case 0x0D:
				case 0x1D:
				case 0x15:
					return ReadRelativeOffset(at + 3, 4, vp);
				}
				break;
			}
			break;

		case 0xE8:
		case 0xE9:
			return ReadRelativeOffset(at + 1, 4, vp);

		case 0xFF:
		case 0x0F:
			switch (ReadMemory<uint8_t>(at + 1, vp))
			{
			case 0x15:
			case 0x25:
			case 0x85:
			case 0x8D:
			case 0x84:
			case 0x8E:
			case 0x82:
			case 0x8C:
				return ReadRelativeOffset(at + 2, 4, vp);
			}
			break;
		}
		return nullptr;
	}

	/*
	 *  MakeJMP
	 *      Creates a JMP instruction at address @at that jumps into address @dest
	 *      If there was already a branch instruction there, returns the previous destination of the branch
	 *      FF25 branch: 14 bytes total; E9 branch: 5 bytes
	 */
	inline memory_pointer_raw MakeJMP(memory_pointer_tr at, memory_pointer_raw dest, bool vp = true)
	{
		auto p = GetBranchDestination(at, vp);

		int64_t offset = static_cast<int64_t>(static_cast<intptr_t>(dest.a))
			- static_cast<int64_t>(static_cast<intptr_t>(at.a + 5));

		if (offset > 0x7FFFFFFF || offset < -static_cast<int64_t>(0x80000000)) {
			WriteMemory<uint8_t>(at, 0xFF, vp);
			WriteMemory<uint8_t>(at + 1, 0x25, vp);
			WriteMemory<uint32_t>(at + 2, 0x0, vp);
			WriteMemory<uintptr_t>(at + 6, dest.a, vp);
		}
		else {
			WriteMemory<uint8_t>(at, 0xE9, vp);
			MakeRelativeOffset(at + 1, dest, 4, vp);
		}

		return p;
	}

	/*
	 *  MakeCALL
	 *      Creates a CALL instruction at address @at that jumps into address @dest
	 */
	inline memory_pointer_raw MakeCALL(memory_pointer_tr at, memory_pointer_raw dest, bool vp = true)
	{
		auto p = GetBranchDestination(at, vp);

		int64_t offset = static_cast<int64_t>(static_cast<intptr_t>(dest.a))
			- static_cast<int64_t>(static_cast<intptr_t>(at.a + 5));

		if (offset > 0x7FFFFFFF || offset < -static_cast<int64_t>(0x80000000)) {
			WriteMemory<uint8_t>(at, 0xFF, vp);
			WriteMemory<uint8_t>(at + 1, 0x15, vp);
			WriteMemory<uint32_t>(at + 2, 0x0, vp);
			WriteMemory<uintptr_t>(at + 6, dest.a, vp);
		}
		else {
			WriteMemory<uint8_t>(at, 0xE8, vp);
			MakeRelativeOffset(at + 1, dest, 4, vp);
		}

		return p;
	}

}
