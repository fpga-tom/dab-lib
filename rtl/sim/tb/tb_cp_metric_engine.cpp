#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <vector>

#include "Vcp_metric_engine.h"
#include "verilated.h"

namespace {

constexpr uint32_t kSampleMask = (1u << 16u) - 1u;
constexpr uint32_t kScoreOneQ30 = 1u << 30u;
constexpr uint32_t kGuard = 8;
constexpr uint32_t kFft = 32;
constexpr double kCfoGain = 159.15494309189535;

struct Sample {
    int32_t i = 0;
    int32_t q = 0;
};

struct HistBeat {
    bool valid = false;
    int32_t i = 0;
    int32_t q = 0;
    uint64_t sample_index = 0;
    bool first = false;
    bool last = false;
    bool error = false;
};

struct ExpectedResp {
    bool ok = false;
    uint64_t fft_start = 0;
    uint32_t index = 0;
    bool last = false;
    int32_t cfo_hz = 0;
    uint32_t min_score = 0;
};

uint32_t pack16(int32_t value) {
    return static_cast<uint32_t>(value) & kSampleMask;
}

int32_t sign_extend_16(uint32_t value) {
    value &= kSampleMask;
    if ((value & (1u << 15u)) != 0u) {
        value |= ~kSampleMask;
    }
    return static_cast<int32_t>(value);
}

int32_t sign_extend_32(uint32_t value) {
    return static_cast<int32_t>(value);
}

Sample base_sample(uint32_t n) {
    const int32_t i = static_cast<int32_t>(((n * 811u) ^ 0x1357u) & 0x1fffu) - 0x1000;
    const int32_t q = static_cast<int32_t>(((n * 1237u) ^ 0x2468u) & 0x1fffu) - 0x1000;
    return {i, q};
}

Sample rotate_sample(const Sample &sample, double theta) {
    const double c = std::cos(theta);
    const double s = std::sin(theta);
    return {static_cast<int32_t>(std::llround(static_cast<double>(sample.i) * c -
                                              static_cast<double>(sample.q) * s)),
            static_cast<int32_t>(std::llround(static_cast<double>(sample.i) * s +
                                              static_cast<double>(sample.q) * c))};
}

class HistoryModel {
public:
    void clear() {
        samples_.clear();
    }

    void set(uint64_t index, Sample sample) {
        samples_[index] = sample;
    }

    std::vector<HistBeat> build_read(uint64_t start, uint32_t length) const {
        std::vector<HistBeat> beats;
        if (length == 0) {
            beats.push_back({true, 0, 0, start, true, true, true});
            return beats;
        }

        for (uint32_t n = 0; n < length; ++n) {
            const uint64_t sample_index = start + n;
            if (samples_.find(sample_index) == samples_.end()) {
                beats.push_back({true, 0, 0, start, true, true, true});
                return beats;
            }
        }

        beats.reserve(length);
        for (uint32_t n = 0; n < length; ++n) {
            const uint64_t sample_index = start + n;
            const Sample sample = samples_.at(sample_index);
            beats.push_back({true,
                             sample.i,
                             sample.q,
                             sample_index,
                             n == 0u,
                             n == length - 1u,
                             false});
        }
        return beats;
    }

private:
    std::unordered_map<uint64_t, Sample> samples_;
};

void load_cp_window(HistoryModel &history,
                    uint64_t fft_start,
                    double theta,
                    bool zero_energy = false) {
    const uint64_t guard_start = fft_start - kGuard;
    const uint64_t useful_tail = fft_start + kFft - kGuard;

    for (uint32_t n = 0; n < kFft + kGuard; ++n) {
        history.set(guard_start + n, base_sample(n + 100));
    }

    for (uint32_t n = 0; n < kGuard; ++n) {
        const Sample prefix = zero_energy ? Sample{0, 0} : base_sample(n + 1);
        const Sample tail = zero_energy ? Sample{0, 0} : rotate_sample(prefix, theta);
        history.set(guard_start + n, prefix);
        history.set(useful_tail + n, tail);
    }
}

void tick(Vcp_metric_engine &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vcp_metric_engine &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.metric_req_valid = 0;
    top.metric_req_fft_start_sample = 0;
    top.metric_req_index = 0;
    top.metric_req_last = 0;
    top.hist_read_req_ready = 0;
    top.hist_rd_out_valid = 0;
    top.hist_rd_out_i = 0;
    top.hist_rd_out_q = 0;
    top.hist_rd_out_sample_index = 0;
    top.hist_rd_out_first = 0;
    top.hist_rd_out_last = 0;
    top.hist_rd_out_error = 0;
    top.metric_resp_ready = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void drive_history(Vcp_metric_engine &top, const std::vector<HistBeat> &beats, size_t pos) {
    if (pos < beats.size()) {
        const HistBeat &beat = beats[pos];
        top.hist_rd_out_valid = beat.valid ? 1 : 0;
        top.hist_rd_out_i = pack16(beat.i);
        top.hist_rd_out_q = pack16(beat.q);
        top.hist_rd_out_sample_index = beat.sample_index;
        top.hist_rd_out_first = beat.first ? 1 : 0;
        top.hist_rd_out_last = beat.last ? 1 : 0;
        top.hist_rd_out_error = beat.error ? 1 : 0;
    } else {
        top.hist_rd_out_valid = 0;
        top.hist_rd_out_i = 0;
        top.hist_rd_out_q = 0;
        top.hist_rd_out_sample_index = 0;
        top.hist_rd_out_first = 0;
        top.hist_rd_out_last = 0;
        top.hist_rd_out_error = 0;
    }
}

void fail(const char *message) {
    std::cerr << message << "\n";
    std::exit(1);
}

void check_response(const Vcp_metric_engine &top, const ExpectedResp &expected) {
    if (!top.metric_resp_valid) {
        fail("Expected metric response valid");
    }
    if (static_cast<bool>(top.metric_resp_ok) != expected.ok) {
        std::cerr << "metric_resp_ok expected=" << expected.ok
                  << " actual=" << static_cast<int>(top.metric_resp_ok) << "\n";
        std::exit(1);
    }
    if (top.metric_resp_fft_start_sample != expected.fft_start) {
        std::cerr << "fft_start expected=" << expected.fft_start
                  << " actual=" << top.metric_resp_fft_start_sample << "\n";
        std::exit(1);
    }
    if (top.metric_resp_index != expected.index) {
        std::cerr << "index expected=" << expected.index
                  << " actual=" << top.metric_resp_index << "\n";
        std::exit(1);
    }
    if (static_cast<bool>(top.metric_resp_last) != expected.last) {
        std::cerr << "last expected=" << expected.last
                  << " actual=" << static_cast<int>(top.metric_resp_last) << "\n";
        std::exit(1);
    }

    if (!expected.ok) {
        if (top.metric_resp_score != 0u || sign_extend_32(top.metric_resp_coarse_freq_error_hz) != 0) {
            fail("Invalid metric response should zero score and CFO");
        }
        return;
    }

    if (top.metric_resp_score < expected.min_score) {
        std::cerr << "score too low expected_min=" << expected.min_score
                  << " actual=" << top.metric_resp_score << "\n";
        std::exit(1);
    }

    const int32_t actual_cfo = sign_extend_32(top.metric_resp_coarse_freq_error_hz);
    if (std::abs(actual_cfo - expected.cfo_hz) > 1) {
        std::cerr << "cfo expected=" << expected.cfo_hz << " actual=" << actual_cfo << "\n";
        std::exit(1);
    }
}

void run_request(Vcp_metric_engine &top,
                 const HistoryModel &history,
                 uint64_t fft_start,
                 uint32_t index,
                 bool last,
                 const ExpectedResp &expected,
                 bool hold_response = false) {
    std::vector<HistBeat> beats;
    size_t beat_pos = 0;
    bool request_sent = false;
    bool response_checked = false;

    for (uint32_t cycle = 0; cycle < 400u; ++cycle) {
        top.metric_req_valid = !request_sent ? 1 : 0;
        top.metric_req_fft_start_sample = fft_start;
        top.metric_req_index = index;
        top.metric_req_last = last ? 1 : 0;
        top.hist_read_req_ready = ((cycle % 4u) != 1u) ? 1 : 0;
        top.metric_resp_ready = hold_response && top.metric_resp_valid && cycle < 8u ? 0 : 1;
        drive_history(top, beats, beat_pos);
        top.eval();

        const bool req_fire = top.metric_req_valid && top.metric_req_ready;
        const bool hist_req_fire = top.hist_read_req_valid && top.hist_read_req_ready;
        const bool hist_fire = top.hist_rd_out_valid && top.hist_rd_out_ready;
        const bool resp_seen = top.metric_resp_valid != 0;
        const bool resp_fire = top.metric_resp_valid && top.metric_resp_ready;

        if (resp_seen && !response_checked) {
            check_response(top, expected);
            response_checked = true;
            if (hold_response && top.metric_resp_ready == 0) {
                top.eval();
                check_response(top, expected);
            }
        }

        tick(top);

        if (req_fire) {
            request_sent = true;
        }
        if (hist_req_fire) {
            beats = history.build_read(top.hist_read_req_start_sample, top.hist_read_req_length);
            beat_pos = 0;
        }
        if (hist_fire) {
            ++beat_pos;
        }
        if (resp_fire && response_checked) {
            return;
        }
    }

    fail("Timed out waiting for CP metric response");
}

void run_clear_test(Vcp_metric_engine &top, const HistoryModel &history) {
    top.metric_req_valid = 1;
    top.metric_req_fft_start_sample = 200;
    top.metric_req_index = 55;
    top.metric_req_last = 0;
    top.hist_read_req_ready = 1;
    top.metric_resp_ready = 1;
    top.clear = 1;
    top.eval();

    if (top.metric_req_ready || top.hist_read_req_valid || top.metric_resp_valid) {
        fail("Clear did not suppress cp_metric_engine handshakes");
    }

    tick(top);
    top.clear = 0;
    top.metric_req_valid = 0;
    top.eval();

    run_request(top,
                history,
                200,
                56,
                false,
                {true, 200, 56, false, 0, kScoreOneQ30 - (kScoreOneQ30 / 64u)});
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vcp_metric_engine top;
    HistoryModel history;

    reset(top);

    load_cp_window(history, 200, 0.0);
    run_request(top,
                history,
                200,
                3,
                false,
                {true, 200, 3, false, 0, kScoreOneQ30 - (kScoreOneQ30 / 64u)},
                true);

    const int32_t cfo_hz = 25;
    const double theta = std::atan(static_cast<double>(cfo_hz) / kCfoGain);
    load_cp_window(history, 300, theta);
    run_request(top,
                history,
                300,
                4,
                true,
                {true, 300, 4, true, cfo_hz, kScoreOneQ30 - (kScoreOneQ30 / 64u)});

    const int32_t neg_cfo_hz = -31;
    const double neg_theta = std::atan(static_cast<double>(neg_cfo_hz) / kCfoGain);
    load_cp_window(history, 350, neg_theta);
    run_request(top,
                history,
                350,
                8,
                false,
                {true, 350, 8, false, neg_cfo_hz, kScoreOneQ30 - (kScoreOneQ30 / 64u)});

    load_cp_window(history, 400, 0.0, true);
    run_request(top, history, 400, 5, false, {false, 400, 5, false, 0, 0});

    run_request(top, history, 3, 6, false, {false, 3, 6, false, 0, 0});

    HistoryModel empty_history;
    run_request(top, empty_history, 600, 7, false, {false, 600, 7, false, 0, 0});

    run_clear_test(top, history);

    std::cout << "cp_metric_engine: PASS\n";
    return 0;
}
