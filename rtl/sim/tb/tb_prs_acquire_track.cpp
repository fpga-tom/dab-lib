#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Vprs_acquire_track.h"
#include "verilated.h"

namespace {

constexpr uint32_t kGuardSamples = 504;
constexpr uint32_t kFftSamples = 2048;
constexpr uint32_t kFrameSamples = 196608;
constexpr uint32_t kAcquireWindow = 1200;
constexpr uint32_t kTrackWindow = 65;
constexpr uint32_t kScoreFracBits = 30;
constexpr uint32_t kNoPilot = 0xffffffffu;

struct Search {
    uint64_t null_start = 0;
    uint32_t null_length = 0;
    uint32_t null_ratio = 0;
    uint64_t prs_fft_start = 0;
    uint32_t window = 0;
};

struct Score {
    bool found = true;
    uint64_t best_fft_start = 0;
    uint32_t score = 0;
    int32_t cfo = 0;
};

struct Pilot {
    bool found = true;
    uint32_t score = 0;
    int32_t carrier_offset = 0;
    int32_t fine_timing_q12 = 0;
    uint32_t used_pairs = 1400;
};

uint32_t score_q(double value) {
    if (value <= 0.0) {
        return 0;
    }
    const double scaled = value * static_cast<double>(uint64_t{1} << kScoreFracBits);
    if (scaled >= static_cast<double>(0xffffffffu)) {
        return 0xffffffffu;
    }
    return static_cast<uint32_t>(scaled);
}

uint32_t pack_signed(int32_t value, uint32_t width) {
    return static_cast<uint32_t>(value) & ((1u << width) - 1u);
}

int32_t sign_extend(uint32_t value, uint32_t width) {
    const uint32_t mask = (1u << width) - 1u;
    value &= mask;
    if ((value & (1u << (width - 1u))) != 0u) {
        value |= ~mask;
    }
    return static_cast<int32_t>(value);
}

int32_t rounded_timing_samples(int32_t q12) {
    if (q12 < 0) {
        return (q12 - 2048) >> 12;
    }
    return (q12 + 2048) >> 12;
}

void tick(Vprs_acquire_track &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void drive_defaults(Vprs_acquire_track &top) {
    top.null_in_valid = 0;
    top.null_in_start_sample = 0;
    top.null_in_length = 0;
    top.null_in_min_ratio = 0;
    top.predict_in_valid = 0;
    top.predict_in_prs_fft_start_sample = 0;
    top.search_out_ready = 0;
    top.score_in_valid = 0;
    top.score_in_found = 0;
    top.score_in_null_start_sample = 0;
    top.score_in_null_length = 0;
    top.score_in_candidate_prs_symbol_start_sample = 0;
    top.score_in_candidate_prs_fft_start_sample = 0;
    top.score_in_timing_search_start_offset = 0;
    top.score_in_timing_search_length = 0;
    top.score_in_search_start_sample = 0;
    top.score_in_search_end_sample = 0;
    top.score_in_required_end_sample = 0;
    top.score_in_prs_peak_sample = 0;
    top.score_in_prs_symbol_start_sample = 0;
    top.score_in_prs_fft_start_sample = 0;
    top.score_in_prs_score = 0;
    top.score_in_coarse_freq_error_hz = 0;
    top.prs_symbol_req_ready = 0;
    top.pilot_in_valid = 0;
    top.pilot_in_found = 0;
    top.pilot_in_symbol_start_sample = 0;
    top.pilot_in_score = 0;
    top.pilot_in_carrier_offset_bins = 0;
    top.pilot_in_fine_timing_error_samples = 0;
    top.pilot_in_used_pairs = 0;
    top.out_ready = 0;
}

void reset(Vprs_acquire_track &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.cfg_enable_tracking = 1;
    top.cfg_min_cp_score = score_q(0.40);
    top.cfg_min_pilot_score = score_q(0.50);
    drive_defaults(top);
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail_at(const char *field, uint64_t expected, uint64_t actual) {
    std::cerr << "Mismatch field=" << field << " expected=" << expected
              << " actual=" << actual << "\n";
    std::exit(1);
}

void check_search(const Vprs_acquire_track &top, const Search &expected) {
    const uint64_t symbol_start = expected.prs_fft_start - kGuardSamples;
    const uint32_t half = expected.window >> 1u;
    const uint64_t search_start =
        expected.prs_fft_start > half ? expected.prs_fft_start - half : 0;

    if (top.search_out_null_start_sample != expected.null_start) {
        fail_at("search_null_start", expected.null_start, top.search_out_null_start_sample);
    }
    if (top.search_out_null_length != expected.null_length) {
        fail_at("search_null_length", expected.null_length, top.search_out_null_length);
    }
    if (top.search_out_null_min_ratio != expected.null_ratio) {
        fail_at("search_null_ratio", expected.null_ratio, top.search_out_null_min_ratio);
    }
    if (top.search_out_prs_symbol_start_sample != symbol_start) {
        fail_at("search_symbol_start", symbol_start, top.search_out_prs_symbol_start_sample);
    }
    if (top.search_out_prs_fft_start_sample != expected.prs_fft_start) {
        fail_at("search_fft_start", expected.prs_fft_start, top.search_out_prs_fft_start_sample);
    }
    if (top.search_out_timing_search_start_offset != half) {
        fail_at("search_offset", half, top.search_out_timing_search_start_offset);
    }
    if (top.search_out_timing_search_length != expected.window) {
        fail_at("search_length", expected.window, top.search_out_timing_search_length);
    }
    if (top.search_out_search_start_sample != search_start) {
        fail_at("search_start", search_start, top.search_out_search_start_sample);
    }
    if (top.search_out_search_end_sample != search_start + expected.window) {
        fail_at("search_end", search_start + expected.window, top.search_out_search_end_sample);
    }
    if (top.search_out_required_end_sample != search_start + expected.window + kFftSamples) {
        fail_at("required_end",
                search_start + expected.window + kFftSamples,
                top.search_out_required_end_sample);
    }
}

void send_null_and_expect_search(Vprs_acquire_track &top,
                                 uint64_t null_start,
                                 uint32_t null_length,
                                 uint32_t ratio,
                                 const Search &expected) {
    bool sent = false;
    for (uint32_t cycle = 0; cycle < 80; ++cycle) {
        drive_defaults(top);
        top.null_in_valid = sent ? 0 : 1;
        top.null_in_start_sample = null_start;
        top.null_in_length = null_length;
        top.null_in_min_ratio = ratio;
        top.search_out_ready = (cycle % 3u) != 1u;
        top.eval();

        const bool input_fire = top.null_in_valid && top.null_in_ready;
        const bool search_fire = top.search_out_valid && top.search_out_ready;

        if (top.search_out_valid) {
            check_search(top, expected);
        }

        tick(top);

        if (input_fire) {
            sent = true;
        }
        if (search_fire) {
            return;
        }
    }
    std::cerr << "Timed out waiting for search from null\n";
    std::exit(1);
}

void send_predict_and_expect_search(Vprs_acquire_track &top,
                                    uint64_t predicted_fft_start,
                                    const Search &expected) {
    bool sent = false;
    for (uint32_t cycle = 0; cycle < 80; ++cycle) {
        drive_defaults(top);
        top.predict_in_valid = sent ? 0 : 1;
        top.predict_in_prs_fft_start_sample = predicted_fft_start;
        top.search_out_ready = (cycle % 4u) != 2u;
        top.eval();

        const bool input_fire = top.predict_in_valid && top.predict_in_ready;
        const bool search_fire = top.search_out_valid && top.search_out_ready;

        if (top.search_out_valid) {
            check_search(top, expected);
        }

        tick(top);

        if (input_fire) {
            sent = true;
        }
        if (search_fire) {
            return;
        }
    }
    std::cerr << "Timed out waiting for predicted search\n";
    std::exit(1);
}

void send_score_and_expect_prs_request(Vprs_acquire_track &top,
                                       const Search &search,
                                       const Score &score) {
    bool score_sent = false;
    for (uint32_t cycle = 0; cycle < 80; ++cycle) {
        drive_defaults(top);
        top.score_in_valid = score_sent ? 0 : 1;
        top.score_in_found = score.found ? 1 : 0;
        top.score_in_null_start_sample = search.null_start;
        top.score_in_null_length = search.null_length;
        top.score_in_candidate_prs_symbol_start_sample = search.prs_fft_start - kGuardSamples;
        top.score_in_candidate_prs_fft_start_sample = search.prs_fft_start;
        top.score_in_timing_search_start_offset = search.window >> 1u;
        top.score_in_timing_search_length = search.window;
        top.score_in_search_start_sample = search.prs_fft_start - (search.window >> 1u);
        top.score_in_search_end_sample = top.score_in_search_start_sample + search.window;
        top.score_in_required_end_sample = top.score_in_search_end_sample + kFftSamples;
        top.score_in_prs_peak_sample = score.best_fft_start;
        top.score_in_prs_symbol_start_sample = score.best_fft_start - kGuardSamples;
        top.score_in_prs_fft_start_sample = score.best_fft_start;
        top.score_in_prs_score = score.score;
        top.score_in_coarse_freq_error_hz = static_cast<uint32_t>(score.cfo);
        top.prs_symbol_req_ready = (cycle % 5u) != 2u;
        top.eval();

        const bool score_fire = top.score_in_valid && top.score_in_ready;
        const bool req_fire = top.prs_symbol_req_valid && top.prs_symbol_req_ready;

        if (top.prs_symbol_req_valid) {
            if (top.prs_symbol_req_symbol_start_sample != score.best_fft_start - kGuardSamples) {
                fail_at("prs_req_symbol_start",
                        score.best_fft_start - kGuardSamples,
                        top.prs_symbol_req_symbol_start_sample);
            }
            if (top.prs_symbol_req_fft_start_sample != score.best_fft_start) {
                fail_at("prs_req_fft_start", score.best_fft_start, top.prs_symbol_req_fft_start_sample);
            }
            if (top.prs_symbol_req_score != score.score) {
                fail_at("prs_req_score", score.score, top.prs_symbol_req_score);
            }
            if (static_cast<int32_t>(top.prs_symbol_req_coarse_freq_error_hz) != score.cfo) {
                fail_at("prs_req_cfo",
                        static_cast<uint32_t>(score.cfo),
                        top.prs_symbol_req_coarse_freq_error_hz);
            }
        }

        tick(top);

        if (score_fire) {
            score_sent = true;
        }
        if (req_fire) {
            return;
        }
    }
    std::cerr << "Timed out waiting for PRS request\n";
    std::exit(1);
}

uint64_t send_pilot_and_expect_output(Vprs_acquire_track &top,
                                      const Score &score,
                                      const Pilot &pilot,
                                      bool expected_locked,
                                      uint32_t expected_lock_count) {
    for (uint32_t cycle = 0; cycle < 80; ++cycle) {
        drive_defaults(top);
        top.pilot_in_valid = 1;
        top.pilot_in_found = pilot.found ? 1 : 0;
        top.pilot_in_symbol_start_sample = score.best_fft_start;
        top.pilot_in_score = pilot.score;
        top.pilot_in_carrier_offset_bins = pack_signed(pilot.carrier_offset, 4);
        top.pilot_in_fine_timing_error_samples = pack_signed(pilot.fine_timing_q12, 24);
        top.pilot_in_used_pairs = pilot.used_pairs;
        top.out_ready = (cycle % 5u) != 1u;
        top.eval();

        const bool output_fire = top.out_valid && top.out_ready;

        if (top.out_valid) {
            const uint64_t expected_next =
                score.best_fft_start + kFrameSamples +
                static_cast<int64_t>(rounded_timing_samples(pilot.fine_timing_q12));
            if (!top.out_found) {
                std::cerr << "Expected successful PRS acquire/track output\n";
                std::exit(1);
            }
            if (top.out_locked != (expected_locked ? 1u : 0u)) {
                fail_at("locked", expected_locked ? 1u : 0u, top.out_locked);
            }
            if (top.out_lock_count != expected_lock_count) {
                fail_at("lock_count", expected_lock_count, top.out_lock_count);
            }
            if (top.out_prs_fft_start_sample != score.best_fft_start) {
                fail_at("out_fft_start", score.best_fft_start, top.out_prs_fft_start_sample);
            }
            if (top.out_prs_symbol_start_sample != score.best_fft_start - kGuardSamples) {
                fail_at("out_symbol_start",
                        score.best_fft_start - kGuardSamples,
                        top.out_prs_symbol_start_sample);
            }
            if (top.out_prs_score != score.score || top.out_pilot_score != pilot.score) {
                fail_at("scores", score.score, top.out_prs_score);
            }
            if (sign_extend(top.out_carrier_offset_bins, 4) != pilot.carrier_offset) {
                fail_at("carrier_offset",
                        static_cast<uint32_t>(pilot.carrier_offset),
                        sign_extend(top.out_carrier_offset_bins, 4));
            }
            if (sign_extend(top.out_fine_timing_error_samples, 24) != pilot.fine_timing_q12) {
                fail_at("fine_timing",
                        static_cast<uint32_t>(pilot.fine_timing_q12),
                        sign_extend(top.out_fine_timing_error_samples, 24));
            }
            if (top.out_next_prs_fft_start_sample != expected_next) {
                fail_at("next_fft", expected_next, top.out_next_prs_fft_start_sample);
            }
        }

        tick(top);

        if (output_fire) {
            return top.out_next_prs_fft_start_sample;
        }
    }
    std::cerr << "Timed out waiting for pilot output\n";
    std::exit(1);
}

bool send_score_miss_and_expect_output(Vprs_acquire_track &top,
                                       const Search &search,
                                       bool expected_locked,
                                       uint32_t expected_miss_count) {
    bool score_sent = false;
    for (uint32_t cycle = 0; cycle < 80; ++cycle) {
        drive_defaults(top);
        top.score_in_valid = score_sent ? 0 : 1;
        top.score_in_found = 0;
        top.score_in_null_start_sample = search.null_start;
        top.score_in_null_length = search.null_length;
        top.score_in_candidate_prs_symbol_start_sample = search.prs_fft_start - kGuardSamples;
        top.score_in_candidate_prs_fft_start_sample = search.prs_fft_start;
        top.score_in_timing_search_start_offset = search.window >> 1u;
        top.score_in_timing_search_length = search.window;
        top.out_ready = (cycle % 3u) != 1u;
        top.eval();

        const bool score_fire = top.score_in_valid && top.score_in_ready;
        const bool output_fire = top.out_valid && top.out_ready;

        if (top.out_valid) {
            if (top.out_found) {
                std::cerr << "Expected miss output\n";
                std::exit(1);
            }
            if (top.out_locked != (expected_locked ? 1u : 0u)) {
                fail_at("miss_locked", expected_locked ? 1u : 0u, top.out_locked);
            }
            if (top.out_miss_count != expected_miss_count) {
                fail_at("miss_count", expected_miss_count, top.out_miss_count);
            }
        }

        tick(top);

        if (score_fire) {
            score_sent = true;
        }
        if (output_fire) {
            return expected_locked;
        }
    }
    std::cerr << "Timed out waiting for miss output\n";
    std::exit(1);
}

void run_clear_test(Vprs_acquire_track &top) {
    drive_defaults(top);
    top.null_in_valid = 1;
    top.null_in_start_sample = 1000;
    top.null_in_length = 2656;
    top.clear = 1;
    top.eval();
    if (top.null_in_ready || top.search_out_valid || top.out_valid) {
        std::cerr << "Clear did not suppress prs_acquire_track interfaces\n";
        std::exit(1);
    }
    tick(top);
    top.clear = 0;
    drive_defaults(top);
    top.eval();
    if (!top.null_in_ready || top.out_valid) {
        std::cerr << "Clear did not return prs_acquire_track to idle\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vprs_acquire_track top;
    reset(top);

    const uint64_t null0 = 100000;
    const uint32_t null_len0 = 2656;
    const uint64_t candidate0 = null0 + null_len0 + kGuardSamples;
    const Search acquire0{null0, null_len0, 0x12345u, candidate0, kAcquireWindow};
    const Score score0{true, candidate0 + 3, score_q(0.81), -37};
    const Pilot pilot0{true, score_q(0.88), 2, 5120, 1430};

    send_null_and_expect_search(top, null0, null_len0, acquire0.null_ratio, acquire0);
    send_score_and_expect_prs_request(top, acquire0, score0);
    uint64_t next_fft = send_pilot_and_expect_output(top, score0, pilot0, false, 1);

    const uint64_t null1 = 296700;
    const uint32_t null_len1 = 2644;
    const uint64_t candidate1 = null1 + null_len1 + kGuardSamples;
    const Search acquire1{null1, null_len1, 0x23456u, candidate1, kAcquireWindow};
    const Score score1{true, candidate1 - 2, score_q(0.84), 19};
    const Pilot pilot1{true, score_q(0.91), -1, 1024, 1420};

    send_null_and_expect_search(top, null1, null_len1, acquire1.null_ratio, acquire1);
    send_score_and_expect_prs_request(top, acquire1, score1);
    next_fft = send_pilot_and_expect_output(top, score1, pilot1, true, 2);

    const Search track0{0, 0, 0, next_fft, kTrackWindow};
    const Score score2{true, next_fft - 1, score_q(0.80), 7};
    const Pilot pilot2{true, score_q(0.86), -2, -3072, 1411};

    send_predict_and_expect_search(top, 0, track0);
    send_score_and_expect_prs_request(top, track0, score2);
    next_fft = send_pilot_and_expect_output(top, score2, pilot2, true, 3);

    const Search miss0{0, 0, 0, next_fft, kTrackWindow};
    send_predict_and_expect_search(top, 0, miss0);
    send_score_miss_and_expect_output(top, miss0, true, 1);

    const Search miss1{0, 0, 0, next_fft, kTrackWindow};
    send_predict_and_expect_search(top, next_fft, miss1);
    send_score_miss_and_expect_output(top, miss1, false, 2);

    run_clear_test(top);

    std::cout << "prs_acquire_track: PASS\n";
    return 0;
}
