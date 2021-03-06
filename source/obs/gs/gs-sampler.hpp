/*
 * Modern effects for a modern Streamer
 * Copyright (C) 2017 Michael Fabian Dirks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#pragma once
#include "common.hpp"

namespace gs {
	class sampler {
		public:
		sampler();
		~sampler();

		void             set_filter(gs_sample_filter v);
		gs_sample_filter get_filter();

		void            set_address_mode_u(gs_address_mode v);
		gs_address_mode get_address_mode_u();

		void            set_address_mode_v(gs_address_mode v);
		gs_address_mode get_address_mode_v();

		void            set_address_mode_w(gs_address_mode v);
		gs_address_mode get_address_mode_w();

		void set_max_anisotropy(std::int32_t v);
		int  get_max_anisotropy();

		void          set_border_color(std::uint32_t v);
		void          set_border_color(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);
		std::uint32_t get_border_color();
		std::uint8_t  get_border_color(bool r, bool g, bool b, bool a);

		gs_sampler_state* refresh();

		gs_sampler_state* get_object();

		private:
		bool              _dirty;
		gs_sampler_info   _sampler_info;
		gs_sampler_state* _sampler_state;
	};
} // namespace gs
