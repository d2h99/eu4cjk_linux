// Core code from Hooking.Patterns
// https://github.com/ThirteenAG/Hooking.Patterns
#include "byte_pattern.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <link.h>

using namespace std;

namespace {

struct PhdrContext
{
	vector<pair<uintptr_t, uintptr_t>>* segments;
	bool found;
};

int phdr_callback(struct dl_phdr_info* info, size_t, void* data)
{
	auto* ctx = static_cast<PhdrContext*>(data);

	if (info->dlpi_name && info->dlpi_name[0] != '\0')
		return 0;

	for (int i = 0; i < info->dlpi_phnum; ++i)
	{
		const auto& ph = info->dlpi_phdr[i];
		if (ph.p_type != PT_LOAD)
			continue;

		uintptr_t beg = info->dlpi_addr + ph.p_vaddr;
		ctx->segments->emplace_back(beg, beg + ph.p_memsz);
	}

	ctx->found = true;
	return 1;
}

}

memory_pointer BytePattern::get(size_t index) const
{
	return this->_results.at(index);
}

memory_pointer BytePattern::get_first() const
{
	return this->get(0);
}

memory_pointer BytePattern::get_second() const
{
	return this->get(1);
}

BytePattern& BytePattern::temp_instance()
{
	static BytePattern instance;

	return instance;
}

BytePattern::BytePattern()
	: _error(false)
{
	set_module();
}

BytePattern& BytePattern::set_pattern(string_view pattern_literal)
{
	this->transform_pattern(pattern_literal);
	this->bm_preprocess();

	return *this;
}

BytePattern& BytePattern::set_module()
{
	this->get_module_ranges();

	return *this;
}

BytePattern& BytePattern::set_range(memory_pointer beg, memory_pointer end)
{
	this->_ranges.clear();
	this->_ranges.emplace_back(beg.address(), end.address());

	return *this;
}

BytePattern& BytePattern::search()
{
	this->bm_search();

	return *this;
}

BytePattern& BytePattern::find_pattern(string_view pattern_literal)
{
	this->set_pattern(pattern_literal).search();

	if (!this->_literal.empty())
	{
		char buf[512];
		snprintf(buf, sizeof(buf), "pattern '%s' -> %zu result(s)", this->_literal.c_str(), this->count());
		log_info(buf);
		for_each_result([](memory_pointer p)
		{
			char line[64];
			snprintf(line, sizeof(line), "  0x%lx", static_cast<unsigned long>(p.address()));
			log_info(line);
		});
	}

	return *this;
}

void BytePattern::log_info(const string& message)
{
	eu4cjk::log_line("%s\n", message.c_str());
}

pair<uint8_t, uint8_t> BytePattern::parse_sub_pattern(string_view sub)
{
	auto digit_to_value = [this](char character) -> int {
		if ('0' <= character && character <= '9') return (character - '0');
		else if ('A' <= character && character <= 'F') return (character - 'A' + 10);
		else if ('a' <= character && character <= 'f') return (character - 'a' + 10);
		this->_error = true;
		return -1; };

	pair<uint8_t, uint8_t> result;

	if (sub.size() == 1)
	{
		if (sub[0] == '?')
		{
			result.first = 0;
			result.second = 0;
		}
		else
		{
			result.first = digit_to_value(sub[0]);
			result.second = 0xFF;
		}
	}
	else if (sub.size() == 2)
	{
		if (sub[0] == '?' && sub[1] == '?')
		{
			result.first = 0;
			result.second = 0;
		}
		else if (sub[0] == '?')
		{
			result.first = digit_to_value(sub[1]);
			result.second = 0xF;
		}
		else if (sub[1] == '?')
		{
			result.first = (digit_to_value(sub[0]) << 4);
			result.second = 0xF0;
		}
		else
		{
			result.first = ((digit_to_value(sub[0]) << 4) | digit_to_value(sub[1]));
			result.second = 0xFF;
		}
	}
	else
	{
		this->_error = true;
	}

	return result;
}

void BytePattern::transform_pattern(string_view literal)
{
	this->clear();
	this->_literal.assign(literal.begin(), literal.end());

	if (literal.empty())
	{
		return;
	}

	size_t pos = 0;
	while (pos <= literal.size())
	{
		size_t next = literal.find(' ', pos);
		string_view sub = literal.substr(pos,
			next == string_view::npos ? literal.size() - pos : next - pos);

		if (!sub.empty())
		{
			auto pat = parse_sub_pattern(sub);

			this->_pattern.push_back(pat.first);
			this->_mask.push_back(pat.second);
		}

		if (next == string_view::npos)
			break;
		pos = next + 1;
	}

	if (this->_error)
	{
		this->clear();
	}
}

void BytePattern::get_module_ranges()
{
	_ranges.clear();

	PhdrContext ctx{&_ranges, false};
	dl_iterate_phdr(phdr_callback, &ctx);

	if (!ctx.found)
		return;

	for (auto& seg : _ranges)
	{
		char buf[128];
		snprintf(buf, sizeof(buf), "phdr seg 0x%lx-0x%lx",
			static_cast<unsigned long>(seg.first), static_cast<unsigned long>(seg.second));
		log_info(buf);
	}

	FILE* maps = fopen("/proc/self/maps", "r");
	if (!maps)
		return;

	vector<pair<uintptr_t, uintptr_t>> segments = std::move(_ranges);
	_ranges.clear();

	char line[1024];
	while (fgets(line, sizeof(line), maps))
	{
		unsigned long start = 0, end = 0;
		char perms[8] = {0};

		if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3)
			continue;

		if (perms[0] != 'r')
			continue;

		for (auto& seg : segments)
		{
			uintptr_t s = max(start, static_cast<unsigned long>(seg.first));
			uintptr_t e = min(end, static_cast<unsigned long>(seg.second));
			if (s < e)
				_ranges.emplace_back(s, e);
		}
	}

	fclose(maps);

	if (_ranges.empty())
		_ranges = std::move(segments);

	for (auto& r : _ranges)
	{
		char buf[128];
		snprintf(buf, sizeof(buf), "scan range 0x%lx-0x%lx",
			static_cast<unsigned long>(r.first), static_cast<unsigned long>(r.second));
		log_info(buf);
	}
}

void BytePattern::clear()
{
	this->_literal.clear();
	this->_pattern.clear();
	this->_mask.clear();
	this->_results.clear();
}

size_t BytePattern::count() const
{
	return this->_results.size();
}

bool BytePattern::has_size(size_t expected, string desc) const
{
	const bool result = (this->_results.size() == expected);

	log_info(desc + (result ? ":[OK]" : ":[NG]"));

	return result;
}

bool BytePattern::empty() const
{
	return this->_results.empty();
}

bool BytePattern::error() const
{
	return this->_error;
}

void BytePattern::bm_preprocess()
{
	ptrdiff_t index;

	const uint8_t* pbytes = this->_pattern.data();
	const uint8_t* pmask = this->_mask.data();
	size_t pattern_len = this->_pattern.size();

	if (pattern_len == 0)
		return;

	for (uint32_t bc = 0; bc < 256; ++bc)
	{
		for (index = pattern_len - 1; index >= 0; --index)
		{
			if ((pbytes[index] & pmask[index]) == (bc & pmask[index]))
			{
				break;
			}
		}

		this->_bmbc[bc] = index;
	}
}

void BytePattern::bm_search()
{
	const uint8_t* pbytes = this->_pattern.data();
	const uint8_t* pmask = this->_mask.data();
	size_t pattern_len = this->_pattern.size();

	this->_results.clear();

	if (pattern_len == 0)
	{
		return;
	}

	for (auto& range : this->_ranges)
	{
		uint8_t* range_begin = reinterpret_cast<uint8_t*>(range.first);
		uint8_t* range_end = reinterpret_cast<uint8_t*>(range.second - pattern_len);

		ptrdiff_t index;

		while (range_begin <= range_end)
		{
			for (index = pattern_len - 1; index >= 0; --index)
			{
				if ((pbytes[index] & pmask[index]) != (range_begin[index] & pmask[index]))
				{
					break;
				}
			}

			if (index == -1)
			{
				this->_results.emplace_back(range_begin);
				range_begin += pattern_len;
			}
			else
			{
				ptrdiff_t shift = index - this->_bmbc[range_begin[index]];
				range_begin += shift > 1 ? shift : 1;
			}
		}
	}
}
