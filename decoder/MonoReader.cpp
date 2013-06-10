/*
	Copyright (c) 2013 Game Closure.  All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	* Redistributions of source code must retain the above copyright notice,
	  this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright notice,
	  this list of conditions and the following disclaimer in the documentation
	  and/or other materials provided with the distribution.
	* Neither the name of GCIF nor the names of its contributors may be used
	  to endorse or promote products derived from this software without
	  specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
	ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
	LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
	CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
	SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
	INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
	CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
	ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
	POSSIBILITY OF SUCH DAMAGE.
*/

#include "MonoReader.hpp"
#include "Enforcer.hpp"
#include "BitMath.hpp"
#include "Filters.hpp"
using namespace cat;

#ifdef CAT_COLLECT_STATS
#include "../encoder/Log.hpp"
#endif

#ifdef CAT_DESYNCH_CHECKS
#define DESYNC_TABLE() \
	CAT_ENFORCE(reader.readWord() == 1234567);
#define DESYNC(x, y) \
	CAT_ENFORCE(reader.readBits(16) == (x ^ 12345)); \
	CAT_ENFORCE(reader.readBits(16) == (y ^ 54321));
#else
#define DESYNC_TABLE()
#define DESYNC(x, y)
#endif


//// MonoReader

void MonoReader::cleanup() {
	if (_filter_decoder) {
		delete _filter_decoder;
		_filter_decoder = 0;
	}
}

int MonoReader::readTables(const Parameters & CAT_RESTRICT params, ImageReader & CAT_RESTRICT reader) {
	// Store parameters
	_params = params;

	// If decoder is disabled,
	if (reader.readBit() == 0) {
		CAT_WARN("Mono") << "Reading row filter tables";

		_use_row_filters = true;

		// If one row filter,
		if (reader.readBit()) {
			_one_row_filter = true;
			_row_filter = reader.readBit();
		} else {
			_one_row_filter = false;
		}

		if CAT_UNLIKELY(!_row_filter_decoder.init(reader)) {
			return GCIF_RE_BAD_MONO;
		}
	} else {
		CAT_WARN("Mono") << "Reading tile-based decoder tables";

		// Enable decoder
		_use_row_filters = false;

		// Calculate bits to represent tile bits field
		u32 range = _params.max_bits - _params.min_bits;
		int bits_bc = 0;
		if (range > 0) {
			bits_bc = BSR32(range) + 1;

			int tile_bits_field_bc = bits_bc;

			// Read tile bits
			_tile_bits_x = reader.readBits(tile_bits_field_bc) + _params.min_bits;

			// Validate input
			if CAT_UNLIKELY(_tile_bits_x > _params.max_bits) {
				CAT_DEBUG_EXCEPTION();
				return GCIF_RE_BAD_MONO;
			}
		} else {
			_tile_bits_x = _params.min_bits;
		}

		DESYNC_TABLE();

		// Initialize tile sizes
		_tile_bits_y = _tile_bits_x;
		_tile_size_x = 1 << _tile_bits_x;
		_tile_size_y = 1 << _tile_bits_y;
		_tile_mask_x = _tile_size_x - 1;
		_tile_mask_y = _tile_size_y - 1;
		_tiles_x = (_params.size_x + _tile_size_x - 1) >> _tile_bits_x;
		_tiles_y = (_params.size_y + _tile_size_y - 1) >> _tile_bits_y;

		_filter_row.resize(_tiles_x);

		_tiles.resize(_tiles_x * _tiles_y);
		_tiles.fill_00();

		// Read palette
		CAT_DEBUG_ENFORCE(MAX_PALETTE == 15);

		// Clear palette so bounds checking does not need to be performed
		CAT_OBJCLR(_palette);

		const int sympal_filter_count = reader.readBits(4);
		for (int ii = 0; ii < sympal_filter_count; ++ii) {
			_palette[ii] = reader.readBits(8);
		}

		DESYNC_TABLE();

		CAT_DEBUG_ENFORCE(MAX_FILTERS == 32);

		// Read normal filters
		_filter_count = reader.readBits(5) + 1;
		for (int ii = 0, iiend = _filter_count; ii < iiend; ++ii) {
			u8 sf = reader.readBits(7);

			// If it is a palette filter,
			if (sf >= SF_COUNT) {
				u8 pf = sf - SF_COUNT;

				CAT_DEBUG_ENFORCE(pf < MAX_PALETTE);

				// Set palette entry
				CAT_WARN("SF") << "Palette " << (int)pf;

				// Set filter function sentinel
				MonoFilterFuncs pal_funcs;
				pal_funcs.safe = pal_funcs.unsafe = (MonoFilterFunc)(pf + 1);
				_sf[ii] = pal_funcs;
			} else {
				_sf[ii] = MONO_FILTERS[sf];
				CAT_WARN("SF") << "Normal " << ii << " = " << (int)sf;
			}
		}

		DESYNC_TABLE();

		CAT_DEBUG_ENFORCE(MAX_CHAOS_LEVELS == 16);

		// Read chaos levels
		int chaos_levels = reader.readBits(4) + 1;
		_chaos.init(chaos_levels, _params.size_x);
		_chaos.start();

		DESYNC_TABLE();

		// For each chaos level,
		for (int ii = 0; ii < chaos_levels; ++ii) {
			if CAT_UNLIKELY(!_decoder[ii].init(reader)) {
				CAT_DEBUG_EXCEPTION();
				return GCIF_RE_BAD_MONO;
			}
		}

		DESYNC_TABLE();

		// Create a recursive decoder if needed
		if (!_filter_decoder) {
			_filter_decoder = new MonoReader;
		}

		Parameters sub_params;
		sub_params.data = _tiles.get();
		sub_params.size_x = _tiles_x;
		sub_params.size_y = _tiles_y;
		sub_params.min_bits = _params.min_bits;
		sub_params.max_bits = _params.max_bits;
		sub_params.num_syms = _filter_count;

		int err;
		if CAT_UNLIKELY((err = _filter_decoder->readTables(sub_params, reader))) {
			return err;
		}

		_current_tile = _tiles.get();
		_current_tile_y = 0;
	}

	DESYNC_TABLE();

	if CAT_UNLIKELY(reader.eof()) {
		CAT_DEBUG_EXCEPTION();
		return GCIF_RE_BAD_MONO;
	}

	_current_row = _params.data;

	return GCIF_RE_OK;
}

int MonoReader::readRowHeader(u16 y, ImageReader & CAT_RESTRICT reader) {
	// If using row filters instead of tiled filters,
	if (_use_row_filters) {
		CAT_DEBUG_ENFORCE(RF_COUNT == 2);

		// If different row filter on each row,
		if (!_one_row_filter) {
			// Read row filter
			_row_filter = reader.readBit();
		}

		// Reset previous to zero
		_prev_filter = 0;
	} else {
		// If at the start of a tile row,
		if ((y & _tile_mask_y) == 0) {
			if (y > 0) {
				// Next tile row
				_current_tile_y++;
				_current_tile += _tiles_x;
			}

			// Read its header too
			_filter_decoder->readRowHeader(_current_tile_y, reader);

			// Clear filter function row
			_filter_row.fill_00();
		}
	}

	if (y > 0) {
		_current_row += _params.size_x;
	}

	DESYNC(0, y);

	CAT_DEBUG_ENFORCE(!reader.eof());

	return GCIF_RE_OK;
}

u8 MonoReader::read(u16 x, u16 y, u8 * CAT_RESTRICT data, ImageReader & CAT_RESTRICT reader) {
	CAT_DEBUG_ENFORCE(x < _params.size_x && y < _params.size_y);

	DESYNC(x, y);

	u16 value;

	// If using row filters,
	if (_use_row_filters) {
		// Read filter residual directly
		value = _row_filter_decoder.next(reader);

		// Defilter the filter value
		if (_row_filter == RF_PREV) {
			const u16 num_syms = _params.num_syms;
			value += _prev_filter;
			if (value >= num_syms) {
				value -= num_syms;
			}
			_prev_filter = value;
		}
	} else {
		// Check cached filter
		const u16 tx = x >> _tile_bits_x;

		// Choose safe/unsafe filter
		MonoFilterFunc filter = _filter_row[tx].safe;

		// If filter must be read,
		if (!filter) {
			const u16 ty = _current_tile_y;
			u8 *tp = _current_tile + tx;
			u8 f = _filter_decoder->read(tx, ty, tp, reader);

			CAT_WARN("FILTER") << (tx << _tile_bits_x) << ", " << (ty << _tile_bits_x) << " : " << (int)f << " at " << x << ", " << y;

			// Read filter
			MonoFilterFuncs * CAT_RESTRICT funcs = &_sf[f];
			_filter_row[tx] = *funcs;
			filter = funcs->safe; // Choose here

			DESYNC(x, y);
		}

		// If the filter is a palette symbol,
#ifdef CAT_WORD_64
		u64 pf = (u64)filter;
#else
		u32 pf = (u32)filter;
#endif
		if (pf <= MAX_PALETTE) {
			CAT_WARN("CAT") << "Reading paletted at " << x << ", " << y;
			value = _palette[pf];

			_chaos.zero(x);
		} else {
			// Get chaos bin
			int chaos = _chaos.get(x);

			// Read residual from bitstream
			u16 residual = _decoder[chaos].next(reader);

			// Store for next chaos lookup
			const u16 num_syms = _params.num_syms;
			_chaos.store(x, residual, num_syms);

			// Calculate predicted value
			u16 pred = filter(data, num_syms, x, y, _params.size_x);

			CAT_WARN("PRED") << pred << " chaos=" << chaos << " residual=" << residual << " num_syms=" << num_syms;

			CAT_DEBUG_ENFORCE(pred < num_syms);

			// Defilter using prediction
			value = residual + pred;
			if (value >= num_syms) {
				value -= num_syms;
			}
		}
	}

	DESYNC(x, y);

	CAT_DEBUG_ENFORCE(!reader.eof());

	*data = (u8)value;
	return value;
}


//// KEEP THIS IN SYNC WITH VERSION ABOVE! ////
// The only change should be that unsafe() is used.

u8 MonoReader::read_unsafe(u16 x, u16 y, u8 * CAT_RESTRICT data, ImageReader & CAT_RESTRICT reader) {
	CAT_DEBUG_ENFORCE(x < _params.size_x && y < _params.size_y);

	DESYNC(x, y);

	u16 value;

	// If using row filters,
	if (_use_row_filters) {
		// Read filter residual directly
		value = _row_filter_decoder.next(reader);

		// Defilter the filter value
		if (_row_filter == RF_PREV) {
			const u16 num_syms = _params.num_syms;
			value += _prev_filter;
			if (value >= num_syms) {
				value -= num_syms;
			}
			_prev_filter = value;
		}
	} else {
		// Check cached filter
		const u16 tx = x >> _tile_bits_x;

		// Choose safe/unsafe filter
		MonoFilterFunc filter = _filter_row[tx].unsafe;

		// If filter must be read,
		if (!filter) {
			const u16 ty = _current_tile_y;
			u8 *tp = _current_tile + tx;
			u8 f = _filter_decoder->read(tx, ty, tp, reader);

			CAT_WARN("FILTER") << (tx << _tile_bits_x) << ", " << (ty << _tile_bits_x) << " : " << (int)f;

			// Read filter
			MonoFilterFuncs * CAT_RESTRICT funcs = &_sf[f];
			_filter_row[tx] = *funcs;
			filter = funcs->unsafe; // Choose here

			DESYNC(x, y);
		}

		// If the filter is a palette symbol,
#ifdef CAT_WORD_64
		u64 pf = (u64)filter;
#else
		u32 pf = (u32)filter;
#endif
		if (pf <= MAX_PALETTE+1) {
			CAT_WARN("CAT") << "Reading paletted at " << x << ", " << y;
			value = _palette[pf - 1];

			_chaos.zero(x);
		} else {
			// Get chaos bin
			int chaos = _chaos.get(x);

			// Read residual from bitstream
			u16 residual = _decoder[chaos].next(reader);

			// Store for next chaos lookup
			const u16 num_syms = _params.num_syms;
			_chaos.store(x, residual, num_syms);

			// Calculate predicted value
			u16 pred = filter(data, num_syms, x, y, _params.size_x);

			CAT_DEBUG_ENFORCE(pred < num_syms);

			// Defilter using prediction
			value = residual + pred;
			if (value >= num_syms) {
				value -= num_syms;
			}
		}
	}

	DESYNC(x, y);

	CAT_DEBUG_ENFORCE(!reader.eof());

	*data = (u8)value;
	return value;
}

//// KEEP THIS IN SYNC WITH VERSION ABOVE! ////


