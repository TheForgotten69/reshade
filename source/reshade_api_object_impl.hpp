/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "reshade_api_device.hpp"
#include <cassert>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace reshade::api
{
	template <typename T, typename... api_object_base>
	class RESHADE_API_NOVTABLE api_object_impl : public api_object_base...
	{
		static_assert(sizeof(T) <= sizeof(uint64_t));

		struct guid_t
		{
			struct hash
			{
				auto operator()(const guid_t &key) const -> size_t
				{
#ifndef _WIN64
					return key.a ^ key.b ^ ((key.c & 0xFF00) | (key.d & 0xFF));
#else
					return key.a ^ (key.b << 1);
#endif
				}
			};
			struct equal
			{
				bool operator()(const guid_t &lhs, const guid_t &rhs) const
				{
#ifndef _WIN64
					return lhs.a == rhs.a && lhs.b == rhs.b && lhs.c == rhs.c && lhs.d == rhs.d;
#else
					return lhs.a == rhs.a && lhs.b == rhs.b;
#endif
				}
			};

#ifndef _WIN64
			uint32_t a, b, c, d;
#else
			uint64_t a, b;
#endif
		};

		static guid_t make_guid(const uint8_t guid[16])
		{
			guid_t result;
			std::memcpy(&result, guid, sizeof(result));
			return result;
		}

	public:
		api_object_impl(const api_object_impl &) = delete;
		api_object_impl &operator=(const api_object_impl &) = delete;

		void get_private_data(const uint8_t guid[16], uint64_t *data) const final
		{
			assert(data != nullptr);

			const guid_t key = make_guid(guid);
			for (const auto &entry : _private_data)
			{
				if (typename guid_t::equal()(entry.first, key))
				{
					*data = entry.second;
					return;
				}
			}
			*data = 0;
		}
		void set_private_data(const uint8_t guid[16], const uint64_t data)  final
		{
			const guid_t key = make_guid(guid);
			const auto it = std::find_if(_private_data.begin(), _private_data.end(), [&key](const std::pair<guid_t, uint64_t> &entry) { return typename guid_t::equal()(entry.first, key); });
			if (data != 0)
			{
				if (it != _private_data.end())
					it->second = data;
				else
					_private_data.emplace_back(key, data);
			}
			else if (it != _private_data.end())
			{
				_private_data.erase(it);
			}
		}

		uint64_t get_native() const final { return (uint64_t)_orig; }

		T _orig;

	protected:
		template <typename... Args>
		explicit api_object_impl(T orig, Args... args) : api_object_base(std::forward<Args>(args)...)..., _orig(orig) {}
		~api_object_impl()
		{
			// All user data should ideally have been removed before destruction, to avoid leaks
			assert(_private_data.empty());
		}

	private:
		std::vector<std::pair<guid_t, uint64_t>> _private_data;
	};
}

template <typename T, size_t STACK_ELEMENTS = 16>
struct temp_mem
{
	explicit temp_mem(size_t elements = STACK_ELEMENTS) : p(stack)
	{
		if (elements > STACK_ELEMENTS)
			p = new T[elements];
	}
	temp_mem(const temp_mem &) = delete;
	temp_mem(temp_mem &&) = delete;
	~temp_mem()
	{
		if (p != stack)
			delete[] p;
	}

	temp_mem &operator=(const temp_mem &) = delete;
	temp_mem &operator=(temp_mem &&other_mem) = delete;

	T &operator[](size_t element)
	{
		assert(element < STACK_ELEMENTS || p != stack);

		return p[element];
	}

	T *p, stack[STACK_ELEMENTS];
};
