#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <vector>

#include "Vnull_detector.h"
#include "verilated.h"

namespace {

constexpr uint32_t kFastWindow = 8;
constexpr uint32_t kSlowWindow = 64;
constexpr uint32_t kPowerWidth = 34;
constexpr uint32_t kRatioWidth = 24;
constexpr uint32_t kRatioFracBits = 22;
constexpr uint32_t kRatioOne = 1u << kRatioFracBits;
constexpr uint32_t kEnterNum = 2;
constexpr uint32_t kEnterDen = 5;
constexpr uint32_t kExitNum = 3;
constexpr uint32_t kExitDen = 5;
constexpr uint32_t kMinNullSamples = 24;
constexpr uint32_t kMaxNullSamples = 96;
constexpr uint32_t kMaxMergeGapSamples = 4;

struct Sample {
    int32_t i;
    int32_t q;
    uint64_t index;
};

struct Event {
    uint64_t sample_index;
    uint32_t length;
    uint64_t avg_power;
    uint32_t min_ratio;
};

uint64_t power_of(const Sample &sample) {
    const int64_t i = sample.i;
    const int64_t q = sample.q;
    return static_cast<uint64_t>((i * i) + (q * q));
}

uint32_t ratio_q(uint64_t fast_sum,
                 uint32_t fast_count,
                 uint64_t slow_sum,
                 uint32_t slow_count) {
    if (fast_count == 0 || slow_count == 0 || slow_sum == 0) {
        return kRatioOne;
    }

    const __uint128_t numerator =
        (__uint128_t{fast_sum} * slow_count) << kRatioFracBits;
    const __uint128_t denominator = __uint128_t{slow_sum} * fast_count;
    const __uint128_t quotient = numerator / denominator;
    const __uint128_t max_ratio = (__uint128_t{1} << kRatioWidth) - 1;
    return static_cast<uint32_t>(quotient > max_ratio ? max_ratio : quotient);
}

bool ratio_less(uint64_t fast_sum,
                uint32_t fast_count,
                uint64_t slow_sum,
                uint32_t slow_count,
                uint32_t threshold_num,
                uint32_t threshold_den) {
    if (fast_count == 0 || slow_count == 0 || slow_sum == 0 ||
        threshold_num == 0 || threshold_den == 0) {
        return false;
    }

    const __uint128_t lhs = __uint128_t{fast_sum} * slow_count * threshold_den;
    const __uint128_t rhs = __uint128_t{slow_sum} * fast_count * threshold_num;
    return lhs < rhs;
}

struct NullModel {
    uint64_t fast_history[kFastWindow]{};
    uint64_t slow_history[kSlowWindow]{};
    uint32_t fast_pos = 0;
    uint32_t slow_pos = 0;
    uint32_t fast_count = 0;
    uint32_t slow_count = 0;
    uint64_t fast_sum = 0;
    uint64_t slow_sum = 0;
    uint64_t run_start_index = 0;
    uint64_t run_power_sum = 0;
    uint32_t run_min_ratio = kRatioOne;
    uint32_t run_length = 0;
    uint32_t pending_gap_length = 0;
    uint64_t pending_gap_power_sum = 0;

    void clear_run() {
        run_length = 0;
        run_power_sum = 0;
        run_min_ratio = kRatioOne;
        pending_gap_length = 0;
        pending_gap_power_sum = 0;
    }

    bool process(const Sample &sample, Event &event) {
        const uint64_t power = power_of(sample);

        if (fast_count == kFastWindow) {
            fast_sum -= fast_history[fast_pos];
        } else {
            ++fast_count;
        }
        fast_history[fast_pos] = power;
        fast_sum += power;
        fast_pos = (fast_pos + 1u) % kFastWindow;

        if (slow_count == kSlowWindow) {
            slow_sum -= slow_history[slow_pos];
        } else {
            ++slow_count;
        }
        slow_history[slow_pos] = power;
        slow_sum += power;
        slow_pos = (slow_pos + 1u) % kSlowWindow;

        const uint32_t ratio = ratio_q(fast_sum, fast_count, slow_sum, slow_count);
        const bool enter = ratio_less(fast_sum,
                                      fast_count,
                                      slow_sum,
                                      slow_count,
                                      kEnterNum,
                                      kEnterDen);
        const bool exit = ratio_less(fast_sum,
                                     fast_count,
                                     slow_sum,
                                     slow_count,
                                     kExitNum,
                                     kExitDen);

        if (run_length == 0) {
            if (enter) {
                run_start_index = sample.index;
                run_power_sum = power;
                run_min_ratio = ratio;
                run_length = 1;
                pending_gap_length = 0;
                pending_gap_power_sum = 0;
            }
            return false;
        }

        if (exit) {
            const uint32_t new_length = pending_gap_length != 0
                ? run_length + pending_gap_length + 1u
                : run_length + 1u;
            const uint64_t new_power_sum = pending_gap_length != 0
                ? run_power_sum + pending_gap_power_sum + power
                : run_power_sum + power;

            if (new_length > (kMaxNullSamples + kMaxMergeGapSamples)) {
                clear_run();
            } else {
                run_length = new_length;
                run_power_sum = new_power_sum;
                if (ratio < run_min_ratio) {
                    run_min_ratio = ratio;
                }
                pending_gap_length = 0;
                pending_gap_power_sum = 0;
            }
            return false;
        }

        ++pending_gap_length;
        pending_gap_power_sum += power;
        if (pending_gap_length > kMaxMergeGapSamples) {
            if (run_length >= kMinNullSamples && run_length <= kMaxNullSamples) {
                event = {
                    run_start_index,
                    run_length,
                    run_power_sum / run_length,
                    run_min_ratio,
                };
                clear_run();
                return true;
            }
            clear_run();
        } else if ((run_length + pending_gap_length) >
                   (kMaxNullSamples + kMaxMergeGapSamples)) {
            clear_run();
        }
        return false;
    }
};

void append_samples(std::vector<Sample> &samples, uint32_t count, int32_t i, int32_t q = 0) {
    for (uint32_t n = 0; n < count; ++n) {
        samples.push_back({i, q, static_cast<uint64_t>(samples.size())});
    }
}

uint32_t mask_width(uint64_t value, uint32_t width) {
    return static_cast<uint32_t>(value & ((uint64_t{1} << width) - 1u));
}

int32_t sign_extend(uint32_t value, uint32_t width) {
    const uint32_t sign_bit = 1u << (width - 1u);
    const uint32_t mask = (1u << width) - 1u;
    value &= mask;
    if ((value & sign_bit) != 0u) {
        value |= ~mask;
    }
    return static_cast<int32_t>(value);
}

void tick(Vnull_detector &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vnull_detector &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.in_valid = 0;
    top.in_i = 0;
    top.in_q = 0;
    top.in_sample_index = 0;
    top.out_ready = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail_at(const char *field, uint32_t event_index, uint64_t expected, uint64_t actual) {
    std::cerr << "Mismatch at event " << event_index << " field=" << field
              << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void check_event(const Vnull_detector &top, const Event &expected, uint32_t event_index) {
    if (top.out_sample_index != expected.sample_index) {
        fail_at("sample_index", event_index, expected.sample_index, top.out_sample_index);
    }
    if (top.out_length != expected.length) {
        fail_at("length", event_index, expected.length, top.out_length);
    }
    if (top.out_avg_power != expected.avg_power) {
        fail_at("avg_power", event_index, expected.avg_power, top.out_avg_power);
    }
    if (top.out_min_ratio != expected.min_ratio) {
        fail_at("min_ratio", event_index, expected.min_ratio, top.out_min_ratio);
    }
}

std::vector<Sample> build_sequence() {
    std::vector<Sample> samples;

    append_samples(samples, 90, 100);
    append_samples(samples, 52, 0);
    append_samples(samples, 35, 100);

    append_samples(samples, 24, 100);
    append_samples(samples, 10, 0);
    append_samples(samples, 25, 100);

    append_samples(samples, 36, 0);
    append_samples(samples, 2, 100);
    append_samples(samples, 35, 0);
    append_samples(samples, 40, 100);

    return samples;
}

void run_sequence(Vnull_detector &top) {
    const std::vector<Sample> samples = build_sequence();
    NullModel model;
    std::deque<Event> expected_events;
    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t cycles = 0;

    while (sent < samples.size() || !expected_events.empty() || top.out_valid) {
        top.out_ready = ((cycles % 11u) != 5u) ? 1 : 0;
        top.in_valid = sent < samples.size() ? 1 : 0;
        top.in_i = sent < samples.size() ? mask_width(samples[sent].i, 16) : 0;
        top.in_q = sent < samples.size() ? mask_width(samples[sent].q, 16) : 0;
        top.in_sample_index = sent < samples.size() ? samples[sent].index : 0;
        top.eval();

        const bool input_fire = top.in_valid && top.in_ready;
        const bool output_fire = top.out_valid && top.out_ready;

        if (output_fire) {
            if (expected_events.empty()) {
                std::cerr << "Unexpected null event at cycle " << cycles << "\n";
                std::exit(1);
            }
            check_event(top, expected_events.front(), received);
            expected_events.pop_front();
            ++received;
        }

        if (input_fire) {
            if (sign_extend(top.in_i, 16) != samples[sent].i ||
                sign_extend(top.in_q, 16) != samples[sent].q) {
                std::cerr << "Input pack mismatch\n";
                std::exit(1);
            }

            Event event{};
            if (model.process(samples[sent], event)) {
                expected_events.push_back(event);
            }
            ++sent;
        }

        tick(top);

        ++cycles;
        if (cycles > samples.size() * 4u) {
            std::cerr << "Timeout sent=" << sent << " received=" << received
                      << " pending_events=" << expected_events.size() << "\n";
            std::exit(1);
        }
    }

    if (received < 2u) {
        std::cerr << "Expected at least two null events, got " << received << "\n";
        std::exit(1);
    }
}

void run_clear_test(Vnull_detector &top) {
    top.out_ready = 0;
    top.in_valid = 1;
    top.in_i = 0;
    top.in_q = 0;
    top.in_sample_index = 1000;
    top.eval();
    tick(top);

    top.clear = 1;
    top.in_valid = 0;
    top.eval();
    if (top.in_ready || top.out_valid) {
        std::cerr << "Clear did not suppress null_detector handshakes\n";
        std::exit(1);
    }
    tick(top);

    top.clear = 0;
    top.eval();
    if (!top.in_ready || top.out_valid) {
        std::cerr << "Clear did not return null_detector to idle\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vnull_detector top;

    reset(top);
    run_sequence(top);
    run_clear_test(top);

    std::cout << "null_detector: PASS\n";
    return 0;
}
