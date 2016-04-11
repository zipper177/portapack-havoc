/*
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc.
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ui_sd_card_debug.hpp"

#include "string_format.hpp"

#include "file.hpp"
#include "lfsr_random.hpp"

#include "ff.h"

#include "ch.h"
#include "hal.h"

class SDCardTestThread {
public:
	enum Result {
		FailCompare = -8,
		FailReadIncomplete = -7,
		FailWriteIncomplete = -6,
		FailAbort = -5,
		FailFileOpenRead = -4,
		FailFileOpenWrite = -3,
		FailHeap = -2,
		FailThread = -1,
		Incomplete = 0,
		OK = 1,
	};

	struct Stats {
		halrtcnt_t write_duration_min { 0 };
		halrtcnt_t write_duration_max { 0 };
		halrtcnt_t write_test_duration { 0 };
		size_t write_bytes { 0 };
		size_t write_count { 0 };

		halrtcnt_t read_duration_min { 0 };
		halrtcnt_t read_duration_max { 0 };
		halrtcnt_t read_test_duration { 0 };
		size_t read_bytes { 0 };
		size_t read_count { 0 };
	};

	SDCardTestThread(
	) {
		thread = chThdCreateFromHeap(NULL, 2048, NORMALPRIO + 10, SDCardTestThread::static_fn, this);
	}

	Result result() const {
		return _result;
	}

	const Stats& stats() const {
		return _stats;
	}

	~SDCardTestThread() {
		chThdTerminate(thread);
		chThdWait(thread);
	}

private:
	static constexpr size_t write_size = 16384;
	static constexpr size_t bytes_to_write = 16 * 1024 * 1024;
	static constexpr size_t bytes_to_read = bytes_to_write;

	static Thread* thread;
	volatile Result _result { Result::Incomplete };
	Stats _stats;

	static msg_t static_fn(void* arg) {
		auto obj = static_cast<SDCardTestThread*>(arg);
		obj->_result = obj->run();
		return 0;
	}

	Result run() {
		const std::string filename { "_PPTEST_.DAT" };

		const auto write_result = write(filename);
		if( write_result != Result::OK ) {
			return write_result;
		}

		if( _stats.write_bytes < bytes_to_write ) {
			return Result::FailWriteIncomplete;
		}

		if( chThdShouldTerminate() ) {
			return Result::FailAbort;
		}

		const auto read_result = read(filename);
		if( read_result != Result::OK ) {
			return read_result;
		}

		f_unlink(filename.c_str());

		if( _stats.read_bytes < bytes_to_read ) {
			return Result::FailReadIncomplete;
		}

		if( chThdShouldTerminate() ) {
			return Result::FailAbort;
		}

		return Result::OK;
	}

	Result write(const std::string& filename) {
		const auto buffer = std::make_unique<std::array<uint8_t, write_size>>();
		if( !buffer ) {
			return Result::FailHeap;
		}

		File file;
		if( !file.open_for_writing(filename) ) {
			return Result::FailFileOpenWrite;
		}

		lfsr_word_t v = 1;

		const halrtcnt_t test_start = halGetCounterValue();
		while( !chThdShouldTerminate() && file.is_ready() && (_stats.write_bytes < bytes_to_write) ) {
			lfsr_fill(v,
				reinterpret_cast<lfsr_word_t*>(buffer->data()),
				sizeof(*buffer.get()) / sizeof(lfsr_word_t)
			);

			const halrtcnt_t write_start = halGetCounterValue();
			if( !file.write(buffer->data(), buffer->size()) ) {
				break;
			}
			const halrtcnt_t write_end = halGetCounterValue();
			_stats.write_bytes += buffer->size();
			_stats.write_count++;

			const halrtcnt_t write_duration = write_end - write_start;
			if( (_stats.write_duration_min == 0) || (write_duration < _stats.write_duration_min) ) {
				_stats.write_duration_min = write_duration;
			}
			if( write_duration > _stats.write_duration_max ) {
				_stats.write_duration_max = write_duration;
			}
		}

		file.close();
		
		const halrtcnt_t test_end = halGetCounterValue();
		_stats.write_test_duration = test_end - test_start;

		return Result::OK;
	}

	Result read(const std::string& filename) {
		const auto buffer = std::make_unique<std::array<uint8_t, write_size>>();
		if( !buffer ) {
			return Result::FailHeap;
		}

		File file;
		if( !file.open_for_reading(filename) ) {
			return Result::FailFileOpenRead;
		}

		lfsr_word_t v = 1;

		const halrtcnt_t test_start = halGetCounterValue();
		while( !chThdShouldTerminate() && file.is_ready() && (_stats.read_bytes < bytes_to_read) ) {
			const halrtcnt_t read_start = halGetCounterValue();
			if( !file.read(buffer->data(), buffer->size()) ) {
				break;
			}
			const halrtcnt_t read_end = halGetCounterValue();
			_stats.read_bytes += buffer->size();
			_stats.read_count++;

			const halrtcnt_t read_duration = read_end - read_start;
			if( (_stats.read_duration_min == 0) || (read_duration < _stats.read_duration_min) ) {
				_stats.read_duration_min = read_duration;
			}
			if( read_duration > _stats.read_duration_max ) {
				_stats.read_duration_max = read_duration;
			}

			if( !lfsr_compare(v,
				reinterpret_cast<lfsr_word_t*>(buffer->data()),
				sizeof(*buffer.get()) / sizeof(lfsr_word_t))
			) {
				return Result::FailCompare;
			}
		}

		file.close();
		
		const halrtcnt_t test_end = halGetCounterValue();
		_stats.read_test_duration = test_end - test_start;

		return Result::OK;
	}
};

Thread* SDCardTestThread::thread { nullptr };

namespace ui {

SDCardDebugView::SDCardDebugView(NavigationView& nav) {
	add_children({ {
		&text_title,
		&text_detected_title,
		&text_detected_value,
		&text_bus_width_title,
		&text_bus_width_value,
		&text_card_mode_title,
		&text_card_mode_value,
		// &text_csd_title,
		// &text_csd_value,
		&text_block_size_title,
		&text_block_size_value,
		&text_block_count_title,
		&text_block_count_value,
		&text_capacity_title,
		&text_capacity_value,
		&text_test_write_time_title,
		&text_test_write_time_value,
		&text_test_write_rate_title,
		&text_test_write_rate_value,
		&text_test_read_time_title,
		&text_test_read_time_value,
		&text_test_read_rate_title,
		&text_test_read_rate_value,
		&button_test,
		&button_ok,
	} });

	button_test.on_select = [this](Button&){ this->on_test(); };
	button_ok.on_select = [&nav](Button&){ nav.pop(); };
}

void SDCardDebugView::on_show() {
	sd_card_status_signal_token = sd_card::status_signal += [this](const sd_card::Status status) {
		this->on_status(status);
	};
	on_status(sd_card::status());
}

void SDCardDebugView::on_hide() {
	sd_card::status_signal -= sd_card_status_signal_token;
}

void SDCardDebugView::focus() {
	button_ok.focus();
}

void SDCardDebugView::on_status(const sd_card::Status) {
	text_bus_width_value.set("");
	text_card_mode_value.set("");
	// text_csd_value.set("");
	text_block_size_value.set("");
	text_block_count_value.set("");
	text_capacity_value.set("");
	text_test_write_time_value.set("");
	text_test_write_rate_value.set("");
	text_test_read_time_value.set("");
	text_test_read_rate_value.set("");

	const bool is_inserted = sdcIsCardInserted(&SDCD1);
	text_detected_value.set(is_inserted ? "Yes" : " No");

	if( is_inserted ) {
		const auto card_width_flags = LPC_SDMMC->CTYPE & 0x10001;
		size_t card_width = 0;
		switch(card_width_flags) {
		case 0x00000: card_width = 1; break;
		case 0x00001: card_width = 4; break;
		case 0x10001: card_width = 8; break;
		default: break;
		}

		text_bus_width_value.set(card_width ? to_string_dec_uint(card_width, 1) : "X");
		text_card_mode_value.set("0x" + to_string_hex(SDCD1.cardmode, 8));
		// text_csd_value.set("0x" + to_string_hex(SDCD1.csd, 8));

		BlockDeviceInfo block_device_info;
		if( sdcGetInfo(&SDCD1, &block_device_info) == CH_SUCCESS ) {
			text_block_size_value.set(to_string_dec_uint(block_device_info.blk_size, 5));
			text_block_count_value.set(to_string_dec_uint(block_device_info.blk_num, 9));
			const uint64_t capacity = block_device_info.blk_size * uint64_t(block_device_info.blk_num);
			if( capacity >= 1000000000 ) {
				const uint32_t capacity_mb = capacity / 1000000U;
				const uint32_t fraction_gb = capacity_mb % 1000;
				const uint32_t capacity_gb = capacity_mb / 1000U;
				text_capacity_value.set(
					to_string_dec_uint(capacity_gb, 3) + "." +
					to_string_dec_uint(fraction_gb, 3, '0') + " GB"
				);
			} else {
				const uint32_t capacity_kb = capacity / 1000U;
				const uint32_t fraction_mb = capacity_kb % 1000;
				const uint32_t capacity_mb = capacity_kb / 1000U;
				text_capacity_value.set(
					to_string_dec_uint(capacity_mb, 3) + "." +
					to_string_dec_uint(fraction_mb, 3, '0') + " MB"
				);
			}
		}
	}
}

static std::string format_ticks_as_ms(const halrtcnt_t value) {
	const uint32_t us = uint64_t(value) * 1000000U / halGetCounterFrequency();
	const uint32_t ms_frac = us % 1000U;
	const uint32_t ms_int = us / 1000U;
	if( ms_int < 1000 ) {
		return to_string_dec_uint(ms_int, 3) + "." + to_string_dec_uint(ms_frac, 3, '0');
	} else {
		return "HHH.HHH";
	}
}

static std::string format_bytes_per_ticks_as_mib(const size_t bytes, const halrtcnt_t ticks) {
	const uint32_t bps = uint64_t(bytes) * halGetCounterFrequency() / ticks;
	const uint32_t kbps = bps / 1000U;
	const uint32_t mbps_frac = kbps % 1000U;
	const uint32_t mbps_int = kbps / 1000U;
	if( mbps_int < 1000 ) {
		return to_string_dec_uint(mbps_int, 3) + "." + to_string_dec_uint(mbps_frac, 3, '0');
	} else {
		return "HHH.HHH";
	}
}

void SDCardDebugView::on_test() {
	text_test_write_time_value.set("");
	text_test_write_rate_value.set("");
	text_test_read_time_value.set("");
	text_test_read_rate_value.set("");

	SDCardTestThread thread;

	// uint32_t spinner_phase = 0;
	while( thread.result() == SDCardTestThread::Result::Incomplete ) {
		chThdSleepMilliseconds(100);

		// spinner_phase += 1;
		// char c = '*';
		// switch(spinner_phase % 4) {
		// case 0: c = '-';  break;
		// case 1: c = '\\'; break;
		// case 2: c = '|';  break;
		// case 3: c = '/';  break;
		// default: c = '*'; break;
		// }
		// text_test_write_value.set({ c });
	}

	if( thread.result() == SDCardTestThread::Result::OK ) {
		const auto stats = thread.stats();
		const auto write_duration_avg = stats.write_test_duration / stats.write_count;

		text_test_write_time_value.set(
			format_ticks_as_ms(stats.write_duration_min) + "/" +
			format_ticks_as_ms(write_duration_avg) + "/" +
			format_ticks_as_ms(stats.write_duration_max)
		);

		text_test_write_rate_value.set(
			format_bytes_per_ticks_as_mib(stats.write_bytes, stats.write_duration_min * stats.write_count) + " " +
			format_bytes_per_ticks_as_mib(stats.write_bytes, stats.write_test_duration)
		);

		const auto read_duration_avg = stats.read_test_duration / stats.read_count;

		text_test_read_time_value.set(
			format_ticks_as_ms(stats.read_duration_min) + "/" +
			format_ticks_as_ms(read_duration_avg) + "/" +
			format_ticks_as_ms(stats.read_duration_max)
		);

		text_test_read_rate_value.set(
			format_bytes_per_ticks_as_mib(stats.read_bytes, stats.read_duration_min * stats.read_count) + " " +
			format_bytes_per_ticks_as_mib(stats.read_bytes, stats.read_test_duration)
		);
	} else {
		text_test_write_time_value.set("Fail: " + to_string_dec_int(toUType(thread.result()), 4));
	}
}

} /* namespace ui */
