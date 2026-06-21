#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <vector>

#include "Vprs_score_search.h"
#include "verilated.h"

namespace {

constexpr uint32_t kGuardSamples = 504;
constexpr uint32_t kScoreFracBits = 30;
constexpr uint32_t kScoreMask = 0xffffffffu;

struct Candidate {
    uint64_t null_start;
    uint32_t null_length;
    uint64_t prs_symbol_start;
    uint64_t prs_fft_start;
    uint32_t timing_offset;
    uint32_t timing_length;
    uint64_t search_start;
    uint64_t search_end;
    uint64_t required_end;
    uint64_t best_fft_start;
    uint32_t best_score;
    int32_t best_cfo;
    bool found;
};

struct PendingResponse {
    uint64_t fft_start;
    uint32_t score;
    int32_t cfo;
    bool ok;
    uint32_t delay;
};

uint32_t score_q(double value) {
    if (value <= 0.0) {
        return 0;
    }
    const double scaled = value * static_cast<double>(uint64_t{1} << kScoreFracBits);
    if (scaled >= static_cast<double>(kScoreMask)) {
        return kScoreMask;
    }
    return static_cast<uint32_t>(scaled);
}

uint32_t score_for(const Candidate &candidate, uint64_t fft_start) {
    if (!candidate.found) {
        return 0;
    }
    if (fft_start == candidate.best_fft_start) {
        return candidate.best_score;
    }
    const uint64_t distance = fft_start > candidate.best_fft_start ?
        fft_start - candidate.best_fft_start : candidate.best_fft_start - fft_start;
    const uint32_t penalty = static_cast<uint32_t>(distance * score_q(0.035));
    return candidate.best_score > penalty ? candidate.best_score - penalty : score_q(0.02);
}

int32_t cfo_for(uint64_t fft_start) {
    return static_cast<int32_t>((static_cast<int64_t>(fft_start & 0x1ffu) - 256) * 17);
}

void tick(Vprs_score_search &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vprs_score_search &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.in_valid = 0;
    top.in_null_start_sample = 0;
    top.in_null_length = 0;
    top.in_prs_symbol_start_sample = 0;
    top.in_prs_fft_start_sample = 0;
    top.in_timing_search_start_offset = 0;
    top.in_timing_search_length = 0;
    top.in_search_start_sample = 0;
    top.in_search_end_sample = 0;
    top.in_required_end_sample = 0;
    top.metric_req_ready = 0;
    top.metric_resp_valid = 0;
    top.metric_resp_ok = 0;
    top.metric_resp_fft_start_sample = 0;
    top.metric_resp_score = 0;
    top.metric_resp_coarse_freq_error_hz = 0;
    top.out_ready = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail_at(const char *field, uint32_t index, int64_t expected, int64_t actual) {
    std::cerr << "Mismatch at result " << index << " field=" << field
              << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void check_result(const Vprs_score_search &top, const Candidate &candidate, uint32_t index) {
    if (top.out_found != (candidate.found ? 1u : 0u)) {
        fail_at("found", index, candidate.found ? 1u : 0u, top.out_found);
    }
    if (top.out_null_start_sample != candidate.null_start) {
        fail_at("null_start", index, candidate.null_start, top.out_null_start_sample);
    }
    if (top.out_null_length != candidate.null_length) {
        fail_at("null_length", index, candidate.null_length, top.out_null_length);
    }
    if (top.out_candidate_prs_symbol_start_sample != candidate.prs_symbol_start) {
        fail_at("candidate_symbol_start",
                index,
                candidate.prs_symbol_start,
                top.out_candidate_prs_symbol_start_sample);
    }
    if (top.out_candidate_prs_fft_start_sample != candidate.prs_fft_start) {
        fail_at("candidate_fft_start",
                index,
                candidate.prs_fft_start,
                top.out_candidate_prs_fft_start_sample);
    }
    if (top.out_timing_search_start_offset != candidate.timing_offset) {
        fail_at("timing_offset", index, candidate.timing_offset, top.out_timing_search_start_offset);
    }
    if (top.out_timing_search_length != candidate.timing_length) {
        fail_at("timing_length", index, candidate.timing_length, top.out_timing_search_length);
    }
    if (top.out_search_start_sample != candidate.search_start) {
        fail_at("search_start", index, candidate.search_start, top.out_search_start_sample);
    }
    if (top.out_search_end_sample != candidate.search_end) {
        fail_at("search_end", index, candidate.search_end, top.out_search_end_sample);
    }
    if (top.out_required_end_sample != candidate.required_end) {
        fail_at("required_end", index, candidate.required_end, top.out_required_end_sample);
    }

    if (!candidate.found) {
        if (top.out_prs_fft_start_sample != 0 || top.out_prs_score != 0) {
            std::cerr << "Invalid candidate should report zero result fields\n";
            std::exit(1);
        }
        return;
    }

    if (top.out_prs_peak_sample != candidate.best_fft_start) {
        fail_at("peak_sample", index, candidate.best_fft_start, top.out_prs_peak_sample);
    }
    if (top.out_prs_fft_start_sample != candidate.best_fft_start) {
        fail_at("fft_start", index, candidate.best_fft_start, top.out_prs_fft_start_sample);
    }
    if (top.out_prs_symbol_start_sample != candidate.best_fft_start - kGuardSamples) {
        fail_at("symbol_start",
                index,
                candidate.best_fft_start - kGuardSamples,
                top.out_prs_symbol_start_sample);
    }
    if (top.out_prs_score != candidate.best_score) {
        fail_at("score", index, candidate.best_score, top.out_prs_score);
    }
    if (top.out_coarse_freq_error_hz != static_cast<uint32_t>(candidate.best_cfo)) {
        fail_at("cfo",
                index,
                static_cast<uint32_t>(candidate.best_cfo),
                top.out_coarse_freq_error_hz);
    }
}

std::vector<Candidate> build_candidates() {
    std::vector<Candidate> candidates;

    Candidate first{};
    first.null_start = 100000;
    first.null_length = 2656;
    first.prs_symbol_start = first.null_start + first.null_length;
    first.prs_fft_start = first.prs_symbol_start + kGuardSamples;
    first.timing_offset = 6;
    first.timing_length = 13;
    first.search_start = first.prs_fft_start - first.timing_offset;
    first.search_end = first.search_start + first.timing_length;
    first.required_end = first.search_end + 2048;
    first.best_fft_start = first.prs_fft_start + 2;
    first.best_score = score_q(0.91);
    first.best_cfo = cfo_for(first.best_fft_start);
    first.found = true;
    candidates.push_back(first);

    Candidate second{};
    second.null_start = 200000;
    second.null_length = 2640;
    second.prs_symbol_start = second.null_start + second.null_length;
    second.prs_fft_start = second.prs_symbol_start + kGuardSamples;
    second.timing_offset = 4;
    second.timing_length = 9;
    second.search_start = second.prs_fft_start - second.timing_offset;
    second.search_end = second.search_start + second.timing_length;
    second.required_end = second.search_end + 2048;
    second.best_fft_start = second.prs_fft_start - 1;
    second.best_score = score_q(0.88);
    second.best_cfo = cfo_for(second.best_fft_start);
    second.found = true;
    candidates.push_back(second);

    Candidate invalid{};
    invalid.null_start = 300000;
    invalid.null_length = 2600;
    invalid.prs_symbol_start = invalid.null_start + invalid.null_length;
    invalid.prs_fft_start = invalid.prs_symbol_start + kGuardSamples;
    invalid.timing_offset = 3;
    invalid.timing_length = 5;
    invalid.search_start = invalid.prs_fft_start - invalid.timing_offset;
    invalid.search_end = invalid.search_start + invalid.timing_length;
    invalid.required_end = invalid.search_end + 2048;
    invalid.found = false;
    candidates.push_back(invalid);

    return candidates;
}

void drive_response(Vprs_score_search &top, std::deque<PendingResponse> &responses) {
    if (!responses.empty() && responses.front().delay == 0) {
        top.metric_resp_valid = 1;
        top.metric_resp_ok = responses.front().ok ? 1 : 0;
        top.metric_resp_fft_start_sample = responses.front().fft_start;
        top.metric_resp_score = responses.front().score;
        top.metric_resp_coarse_freq_error_hz =
            static_cast<uint32_t>(responses.front().cfo);
    } else {
        top.metric_resp_valid = 0;
        top.metric_resp_ok = 0;
        top.metric_resp_fft_start_sample = 0;
        top.metric_resp_score = 0;
        top.metric_resp_coarse_freq_error_hz = 0;
    }
}

void run_candidates(Vprs_score_search &top) {
    const std::vector<Candidate> candidates = build_candidates();
    std::deque<PendingResponse> responses;
    uint32_t sent = 0;
    uint32_t active_candidate = 0;
    uint32_t received = 0;
    uint32_t request_count_for_candidate = 0;
    uint32_t cycles = 0;

    while (received < candidates.size()) {
        const Candidate &candidate = candidates[sent < candidates.size() ? sent : candidates.size() - 1u];
        top.in_valid = sent < candidates.size() ? 1 : 0;
        top.in_null_start_sample = candidate.null_start;
        top.in_null_length = candidate.null_length;
        top.in_prs_symbol_start_sample = candidate.prs_symbol_start;
        top.in_prs_fft_start_sample = candidate.prs_fft_start;
        top.in_timing_search_start_offset = candidate.timing_offset;
        top.in_timing_search_length = candidate.timing_length;
        top.in_search_start_sample = candidate.search_start;
        top.in_search_end_sample = candidate.search_end;
        top.in_required_end_sample = candidate.required_end;
        top.metric_req_ready = ((cycles % 5u) != 2u) ? 1 : 0;
        top.out_ready = ((cycles % 7u) != 4u) ? 1 : 0;

        if (!responses.empty() && responses.front().delay > 0) {
            --responses.front().delay;
        }
        drive_response(top, responses);
        top.eval();

        const bool input_fire = top.in_valid && top.in_ready;
        const bool req_fire = top.metric_req_valid && top.metric_req_ready;
        const bool resp_fire = top.metric_resp_valid && top.metric_resp_ready;
        const bool output_fire = top.out_valid && top.out_ready;

        if (input_fire) {
            active_candidate = sent;
            request_count_for_candidate = 0;
            ++sent;
        }

        if (req_fire) {
            const Candidate &active = candidates[active_candidate];
            const uint64_t expected_fft = active.search_start + request_count_for_candidate;
            if (top.metric_req_fft_start_sample != expected_fft) {
                fail_at("metric_req_fft",
                        active_candidate,
                        expected_fft,
                        top.metric_req_fft_start_sample);
            }
            if (top.metric_req_index != request_count_for_candidate) {
                fail_at("metric_req_index",
                        active_candidate,
                        request_count_for_candidate,
                        top.metric_req_index);
            }
            if (top.metric_req_last !=
                (request_count_for_candidate + 1u == active.timing_length ? 1u : 0u)) {
                fail_at("metric_req_last",
                        active_candidate,
                        request_count_for_candidate + 1u == active.timing_length ? 1u : 0u,
                        top.metric_req_last);
            }

            responses.push_back({
                expected_fft,
                score_for(active, expected_fft),
                cfo_for(expected_fft),
                active.found,
                static_cast<uint32_t>((request_count_for_candidate % 3u) + 1u),
            });
            ++request_count_for_candidate;
        }

        if (output_fire) {
            check_result(top, candidates[received], received);
            ++received;
        }

        tick(top);

        if (resp_fire) {
            responses.pop_front();
        }

        ++cycles;
        if (cycles > 1000u) {
            std::cerr << "Timeout sent=" << sent << " received=" << received
                      << " responses=" << responses.size() << "\n";
            std::exit(1);
        }
    }

    top.in_valid = 0;
    top.out_ready = 1;
    tick(top);
    top.eval();
    if (top.out_valid || !top.in_ready) {
        std::cerr << "prs_score_search did not return to idle\n";
        std::exit(1);
    }
}

void run_clear_test(Vprs_score_search &top) {
    const Candidate candidate = build_candidates().front();

    top.in_valid = 1;
    top.in_null_start_sample = candidate.null_start;
    top.in_null_length = candidate.null_length;
    top.in_prs_symbol_start_sample = candidate.prs_symbol_start;
    top.in_prs_fft_start_sample = candidate.prs_fft_start;
    top.in_timing_search_start_offset = candidate.timing_offset;
    top.in_timing_search_length = candidate.timing_length;
    top.in_search_start_sample = candidate.search_start;
    top.in_search_end_sample = candidate.search_end;
    top.in_required_end_sample = candidate.required_end;
    top.metric_req_ready = 0;
    top.metric_resp_valid = 0;
    top.out_ready = 0;
    top.eval();
    tick(top);

    if (!top.metric_req_valid || top.in_ready) {
        std::cerr << "Clear setup did not enter active search\n";
        std::exit(1);
    }

    top.clear = 1;
    top.in_valid = 0;
    top.eval();
    if (top.in_ready || top.metric_req_valid || top.metric_resp_ready) {
        std::cerr << "Clear did not suppress active handshakes\n";
        std::exit(1);
    }
    tick(top);

    top.clear = 0;
    top.eval();
    if (!top.in_ready || top.out_valid || top.metric_req_valid) {
        std::cerr << "Clear did not return prs_score_search to idle\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vprs_score_search top;

    reset(top);
    run_candidates(top);
    run_clear_test(top);
    run_candidates(top);

    std::cout << "prs_score_search: PASS\n";
    return 0;
}
