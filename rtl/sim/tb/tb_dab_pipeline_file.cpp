#include <algorithm>
#include <cerrno>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(DAB_TB_CUSTOM_FFT)
#include "Vdab_pipeline_top_custom_fft.h"
using TopType = Vdab_pipeline_top_custom_fft;
#else
#include "Vdab_pipeline_top.h"
using TopType = Vdab_pipeline_top;
#endif
#include "verilated.h"

namespace {

constexpr uint32_t kFftSamples = 2048;
constexpr uint32_t kGuardSamples = 504;
constexpr uint32_t kSymbolSamples = 2552;
constexpr uint32_t kTotalSymbols = 75;
constexpr uint32_t kSoftbitsPerSymbol = 3072;
constexpr uint32_t kFrameSoftbits = kTotalSymbols * kSoftbitsPerSymbol;
constexpr uint32_t kScoreFracBits = 30;
constexpr uint32_t kCarrierWidth = 24;
constexpr uint32_t kSampleWidth = 16;
constexpr double kTwoPi = 6.283185307179586476925286766559;

struct Sample {
    int16_t i = 0;
    int16_t q = 0;
};

struct FftBin {
    int32_t i = 0;
    int32_t q = 0;
};

struct Config {
    std::string iq_path = "/tmp/dab_iq_f32.bin";
    std::string softbits_path = "/tmp/dab_softbits_i8.bin";
    uint64_t max_cycles = 200000000ull;
    uint64_t sample_limit = 0;
    uint64_t log_every_cycles = 0;
    uint32_t target_tframes = 1;
    bool require_frame = true;
    bool stop_feed_after_frame_ready = true;
    bool internal_data_scheduler = true;
    bool streaming_cp_search = true;
    uint32_t sample_period_cycles = 1;
    double min_cp_score = 0.40;
    double min_pilot_score = 0.50;
    double fft_target_rms = static_cast<double>(1u << 22u);
    bool fft_fixed_gain = false;
    double fft_gain = 1.0;
};

struct CycleFires {
    bool sdr = false;
    bool fft_in = false;
    bool fft_in_last = false;
    int32_t fft_in_i = 0;
    int32_t fft_in_q = 0;
    bool fft_out = false;
    bool data_cmd = false;
    bool sync = false;
    bool sync_found = false;
    bool sync_locked = false;
    uint64_t sync_prs_fft_start = 0;
    uint32_t sync_prs_score = 0;
    uint32_t sync_pilot_score = 0;
    int32_t sync_carrier_offset = 0;
    int32_t sync_fine_timing = 0;
    bool softbit = false;
    int8_t softbit_value = 0;
    bool frame_done = false;
    uint64_t frame_start_sample = 0;
};

uint64_t env_u64(const char *name, uint64_t fallback) {
    const char *value = std::getenv(name);
    char *end = nullptr;

    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return fallback;
    }
    return static_cast<uint64_t>(parsed);
}

double env_f64(const char *name, double fallback) {
    const char *value = std::getenv(name);
    char *end = nullptr;

    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    const double parsed = std::strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0') {
        return fallback;
    }
    return parsed;
}

bool env_bool(const char *name, bool fallback) {
    const char *value = std::getenv(name);

    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    if (std::strcmp(value, "1") == 0 ||
        std::strcmp(value, "true") == 0 ||
        std::strcmp(value, "yes") == 0 ||
        std::strcmp(value, "on") == 0) {
        return true;
    }
    if (std::strcmp(value, "0") == 0 ||
        std::strcmp(value, "false") == 0 ||
        std::strcmp(value, "no") == 0 ||
        std::strcmp(value, "off") == 0) {
        return false;
    }
    return fallback;
}

std::string env_string(const char *name, const std::string &fallback) {
    const char *value = std::getenv(name);
    return (value == nullptr || value[0] == '\0') ? fallback : std::string(value);
}

Config load_config() {
    Config cfg;

    cfg.iq_path = env_string("DAB_TB_IQ_PATH", cfg.iq_path);
    cfg.softbits_path = env_string("DAB_TB_SOFTBITS_PATH", cfg.softbits_path);
    cfg.max_cycles = env_u64("DAB_TB_MAX_CYCLES", cfg.max_cycles);
    cfg.sample_limit = env_u64("DAB_TB_SAMPLE_LIMIT", cfg.sample_limit);
    cfg.log_every_cycles = env_u64("DAB_TB_LOG_EVERY_CYCLES", cfg.log_every_cycles);
    cfg.target_tframes = static_cast<uint32_t>(env_u64("DAB_TB_TFRAMES", cfg.target_tframes));
    if (cfg.target_tframes == 0u) {
        cfg.target_tframes = 1u;
    }
    cfg.require_frame = env_bool("DAB_TB_REQUIRE_FRAME", cfg.require_frame);
    cfg.internal_data_scheduler =
        env_bool("DAB_TB_INTERNAL_DATA_SCHEDULER", cfg.internal_data_scheduler);
    cfg.streaming_cp_search =
        env_bool("DAB_TB_ENABLE_STREAMING_CP_SEARCH", cfg.streaming_cp_search);
    cfg.stop_feed_after_frame_ready =
        env_bool("DAB_TB_STOP_FEED_AFTER_FRAME_READY",
                 !cfg.internal_data_scheduler && cfg.target_tframes <= 1u);
    cfg.sample_period_cycles = static_cast<uint32_t>(
        env_u64("DAB_TB_SAMPLE_PERIOD_CYCLES",
                cfg.internal_data_scheduler ? (cfg.streaming_cp_search ? 4u : 16u) : 1u));
    if (cfg.sample_period_cycles == 0u) {
        cfg.sample_period_cycles = 1u;
    }
    cfg.min_cp_score = env_f64("DAB_TB_MIN_CP_SCORE", cfg.min_cp_score);
    cfg.min_pilot_score = env_f64("DAB_TB_MIN_PILOT_SCORE", cfg.min_pilot_score);
    cfg.fft_target_rms = env_f64("DAB_TB_FFT_TARGET_RMS", cfg.fft_target_rms);
    cfg.fft_gain = env_f64("DAB_TB_FFT_GAIN", cfg.fft_gain);
    cfg.fft_fixed_gain = std::getenv("DAB_TB_FFT_GAIN") != nullptr;
    return cfg;
}

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

double score_to_float(uint32_t score) {
    return static_cast<double>(score) / static_cast<double>(uint64_t{1} << kScoreFracBits);
}

int64_t sign_extend(uint64_t value, uint32_t bits) {
    const uint64_t mask = bits == 64u ? ~uint64_t{0} : ((uint64_t{1} << bits) - 1u);
    value &= mask;
    if ((value & (uint64_t{1} << (bits - 1u))) != 0u) {
        value |= ~mask;
    }
    return static_cast<int64_t>(value);
}

uint32_t pack_signed(int64_t value, uint32_t bits) {
    const uint64_t mask = bits == 32u ? 0xffffffffull : ((uint64_t{1} << bits) - 1u);
    return static_cast<uint32_t>(static_cast<uint64_t>(value) & mask);
}

int16_t clamp_i16(float value) {
    const long rounded = std::lround(value);
    if (rounded > std::numeric_limits<int16_t>::max()) {
        return std::numeric_limits<int16_t>::max();
    }
    if (rounded < std::numeric_limits<int16_t>::min()) {
        return std::numeric_limits<int16_t>::min();
    }
    return static_cast<int16_t>(rounded);
}

int32_t clamp_signed_width(int64_t value, uint32_t bits, uint64_t *saturations) {
    const int64_t max_value = (int64_t{1} << (bits - 1u)) - 1;
    const int64_t min_value = -(int64_t{1} << (bits - 1u));

    if (value > max_value) {
        if (saturations != nullptr) {
            ++(*saturations);
        }
        return static_cast<int32_t>(max_value);
    }
    if (value < min_value) {
        if (saturations != nullptr) {
            ++(*saturations);
        }
        return static_cast<int32_t>(min_value);
    }
    return static_cast<int32_t>(value);
}

std::vector<Sample> read_iq_f32_file(const Config &cfg) {
    std::ifstream input(cfg.iq_path.c_str(), std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("failed to open IQ file: " + cfg.iq_path);
    }

    const std::streamoff size = input.tellg();
    if (size < 0 || (size % static_cast<std::streamoff>(2u * sizeof(float))) != 0) {
        throw std::runtime_error("IQ file size is not a multiple of interleaved float32 IQ");
    }

    uint64_t sample_count =
        static_cast<uint64_t>(size) / static_cast<uint64_t>(2u * sizeof(float));
    if (cfg.sample_limit != 0 && cfg.sample_limit < sample_count) {
        sample_count = cfg.sample_limit;
    }

    std::vector<Sample> samples;
    samples.resize(static_cast<size_t>(sample_count));

    input.seekg(0, std::ios::beg);
    for (uint64_t n = 0; n < sample_count; ++n) {
        float iq[2] = {0.0f, 0.0f};
        input.read(reinterpret_cast<char *>(iq), sizeof(iq));
        if (!input) {
            throw std::runtime_error("failed while reading IQ file: " + cfg.iq_path);
        }
        samples[static_cast<size_t>(n)].i = clamp_i16(iq[0]);
        samples[static_cast<size_t>(n)].q = clamp_i16(iq[1]);
    }
    return samples;
}

class FftShim {
public:
    explicit FftShim(const Config &cfg)
        : target_rms_(cfg.fft_target_rms),
          fixed_gain_(cfg.fft_fixed_gain),
          fixed_gain_value_(cfg.fft_gain) {
        input_.reserve(kFftSamples);
    }

    bool can_accept() const {
        return input_.size() < kFftSamples && output_frames_.size() < kMaxQueuedOutputFrames;
    }

    bool has_output() const {
        return !output_frames_.empty();
    }

    FftBin current_output() const {
        return output_frames_.front()[output_pos_];
    }

    void pop_output() {
        if (!output_frames_.empty()) {
            ++output_pos_;
            if (output_pos_ == output_frames_.front().size()) {
                output_frames_.pop_front();
                output_pos_ = 0;
            }
        }
    }

    void push_input(int32_t i, int32_t q, bool last) {
        if (!can_accept()) {
            throw std::runtime_error("FFT shim accepted input while busy");
        }

        input_.push_back(std::complex<double>(static_cast<double>(i), static_cast<double>(q)));
        if (last) {
            if (input_.size() != kFftSamples) {
                throw std::runtime_error("FFT core input TLAST before 2048 samples");
            }
            compute_output();
            input_.clear();
        } else if (input_.size() == kFftSamples) {
            throw std::runtime_error("FFT core input reached 2048 samples without TLAST");
        }
    }

    uint64_t fft_count() const {
        return fft_count_;
    }

    uint64_t saturation_count() const {
        return saturation_count_;
    }

    double last_scale() const {
        return last_scale_;
    }

    double max_raw_bin_abs() const {
        return max_raw_bin_abs_;
    }

private:
    static constexpr size_t kMaxQueuedOutputFrames = 16;

    static uint32_t reverse_bits(uint32_t value, uint32_t bits) {
        uint32_t reversed = 0;
        for (uint32_t bit = 0; bit < bits; ++bit) {
            reversed = (reversed << 1u) | ((value >> bit) & 1u);
        }
        return reversed;
    }

    static void fft_in_place(std::vector<std::complex<double>> &x) {
        constexpr uint32_t kBits = 11;

        for (uint32_t i = 0; i < kFftSamples; ++i) {
            const uint32_t j = reverse_bits(i, kBits);
            if (j > i) {
                std::swap(x[i], x[j]);
            }
        }

        for (uint32_t len = 2; len <= kFftSamples; len <<= 1u) {
            const uint32_t half = len >> 1u;
            const double angle_step = -kTwoPi / static_cast<double>(len);
            for (uint32_t base = 0; base < kFftSamples; base += len) {
                for (uint32_t j = 0; j < half; ++j) {
                    const double angle = angle_step * static_cast<double>(j);
                    const std::complex<double> w(std::cos(angle), std::sin(angle));
                    const std::complex<double> u = x[base + j];
                    const std::complex<double> v = x[base + j + half] * w;
                    x[base + j] = u + v;
                    x[base + j + half] = u - v;
                }
            }
        }
    }

    double active_rms(const std::vector<std::complex<double>> &x) const {
        double energy = 0.0;
        uint32_t count = 0;

        for (uint32_t bin = kFftSamples - 768u; bin < kFftSamples; ++bin) {
            energy += std::norm(x[bin]);
            ++count;
        }
        for (uint32_t bin = 1; bin <= 768u; ++bin) {
            energy += std::norm(x[bin]);
            ++count;
        }

        if (count == 0 || energy <= 0.0) {
            return 0.0;
        }
        return std::sqrt(energy / static_cast<double>(count));
    }

    void compute_output() {
        std::vector<std::complex<double>> bins = input_;
        fft_in_place(bins);

        double raw_max = 0.0;
        for (const std::complex<double> &bin : bins) {
            raw_max = std::max(raw_max, std::abs(bin));
        }
        max_raw_bin_abs_ = std::max(max_raw_bin_abs_, raw_max);

        const double rms = active_rms(bins);
        const double scale = fixed_gain_ ? fixed_gain_value_ :
                             (rms > 0.0 ? target_rms_ / rms : 0.0);
        last_scale_ = scale;

        std::vector<FftBin> output;
        output.resize(kFftSamples);
        for (uint32_t n = 0; n < kFftSamples; ++n) {
            const int64_t re = static_cast<int64_t>(std::llround(bins[n].real() * scale));
            const int64_t im = static_cast<int64_t>(std::llround(bins[n].imag() * scale));
            output[n].i = clamp_signed_width(re, kCarrierWidth, &saturation_count_);
            output[n].q = clamp_signed_width(im, kCarrierWidth, &saturation_count_);
        }
        output_frames_.push_back(std::move(output));
        ++fft_count_;
    }

    std::vector<std::complex<double>> input_;
    std::deque<std::vector<FftBin>> output_frames_;
    size_t output_pos_ = 0;
    double target_rms_ = static_cast<double>(1u << 22u);
    bool fixed_gain_ = false;
    double fixed_gain_value_ = 1.0;
    double last_scale_ = 0.0;
    double max_raw_bin_abs_ = 0.0;
    uint64_t fft_count_ = 0;
    uint64_t saturation_count_ = 0;
};

class DataScheduler {
public:
    bool observe_sync(const CycleFires &fires) {
        if (!fires.sync || !fires.sync_found || has_frame(fires.sync_prs_fft_start)) {
            return false;
        }

        Frame frame;

        frame.prs_fft_start = fires.sync_prs_fft_start;
        frame.required_end_sample =
            frame.prs_fft_start + (uint64_t{kTotalSymbols} * kSymbolSamples) + kFftSamples;
        pending_.push_back(frame);
        ++observed_frames_;
        return true;
    }

    bool can_arm(const TopType &top) const {
        if (pending_.empty() || armed_ || waiting_for_frame_done_) {
            return false;
        }
        const Frame &frame = pending_.front();
        return top.history_valid &&
               top.history_oldest_sample_index <= frame.prs_fft_start &&
               top.history_next_sample_index >= frame.required_end_sample;
    }

    void arm() {
        if (pending_.empty()) {
            return;
        }
        current_ = pending_.front();
        pending_.pop_front();
        armed_ = true;
        next_symbol_ = 0;
    }

    bool command_valid() const {
        return armed_ && next_symbol_ < kTotalSymbols;
    }

    uint64_t command_fft_start() const {
        return current_.prs_fft_start + (uint64_t{next_symbol_ + 1u} * kSymbolSamples);
    }

    void command_accepted() {
        if (next_symbol_ < kTotalSymbols) {
            ++next_symbol_;
        }
        if (next_symbol_ == kTotalSymbols) {
            armed_ = false;
            waiting_for_frame_done_ = true;
            ++scheduled_frames_;
        }
    }

    void frame_done() {
        waiting_for_frame_done_ = false;
    }

    bool armed() const {
        return armed_;
    }

    bool waiting_for_frame_done() const {
        return waiting_for_frame_done_;
    }

    uint32_t next_symbol() const {
        return next_symbol_;
    }

    uint64_t prs_fft_start() const {
        return current_.prs_fft_start;
    }

    uint64_t frame_required_end_sample() const {
        return current_.required_end_sample;
    }

    uint64_t observed_frames() const {
        return observed_frames_;
    }

    uint64_t scheduled_frames() const {
        return scheduled_frames_;
    }

    size_t pending_count() const {
        return pending_.size();
    }

private:
    struct Frame {
        uint64_t prs_fft_start = 0;
        uint64_t required_end_sample = 0;
    };

    bool has_frame(uint64_t prs_fft_start) const {
        size_t i;

        if ((armed_ || waiting_for_frame_done_) && current_.prs_fft_start == prs_fft_start) {
            return true;
        }
        for (i = 0; i < pending_.size(); ++i) {
            if (pending_[i].prs_fft_start == prs_fft_start) {
                return true;
            }
        }
        return false;
    }

    std::deque<Frame> pending_;
    Frame current_;
    bool armed_ = false;
    bool waiting_for_frame_done_ = false;
    uint32_t next_symbol_ = 0;
    uint64_t observed_frames_ = 0;
    uint64_t scheduled_frames_ = 0;
};

void drive_quiescent(TopType &top, const Config &cfg) {
    top.clear = 0;
    top.set_sample_index_valid = 0;
    top.set_sample_index_value = 0;
    top.sdr_in_valid = 0;
    top.sdr_in_i = 0;
    top.sdr_in_q = 0;
    top.cfg_enable_tracking = 1;
    top.cfg_min_cp_score = score_q(cfg.min_cp_score);
    top.cfg_min_pilot_score = score_q(cfg.min_pilot_score);
    top.cfg_cfo_bypass = 1;
    top.cfg_cfo_phase_inc = 0;
    top.cfg_cfo_phase_load = 0;
    top.cfg_cfo_phase_seed = 0;
    top.predict_in_valid = 0;
    top.predict_in_prs_fft_start_sample = 0;
    top.data_symbol_cmd_valid = 0;
    top.data_symbol_cmd_fft_start_sample = 0;
#if !defined(DAB_TB_CUSTOM_FFT)
    top.fft_core_in_ready = 0;
    top.fft_core_out_valid = 0;
    top.fft_core_out_i = 0;
    top.fft_core_out_q = 0;
    top.fft_core_out_block_exp = 0;
    top.fft_core_out_overflow = 0;
#endif
    top.sync_out_ready = 1;
    top.tframe_out_ready = 1;
}

void raw_tick(TopType &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset_top(TopType &top, const Config &cfg) {
    drive_quiescent(top, cfg);
    top.rst_n = 0;
    raw_tick(top);
    raw_tick(top);

    top.rst_n = 1;
    raw_tick(top);

    drive_quiescent(top, cfg);
    top.set_sample_index_valid = 1;
    top.set_sample_index_value = 0;
    raw_tick(top);

    drive_quiescent(top, cfg);
    raw_tick(top);
}

void drive_cycle_inputs(TopType &top,
                        const Config &cfg,
#if !defined(DAB_TB_CUSTOM_FFT)
                        const FftShim &fft,
#endif
                        const DataScheduler &scheduler,
                        const std::vector<Sample> &samples,
                        uint64_t sample_pos,
                        bool feed_enabled,
                        bool sample_cycle) {
    drive_quiescent(top, cfg);

    if (feed_enabled && sample_cycle && sample_pos < samples.size()) {
        const Sample &sample = samples[static_cast<size_t>(sample_pos)];
        top.sdr_in_valid = 1;
        top.sdr_in_i = pack_signed(sample.i, kSampleWidth);
        top.sdr_in_q = pack_signed(sample.q, kSampleWidth);
    }

#if !defined(DAB_TB_CUSTOM_FFT)
    top.fft_core_in_ready = fft.can_accept() ? 1 : 0;
    if (fft.has_output()) {
        const FftBin bin = fft.current_output();
        top.fft_core_out_valid = 1;
        top.fft_core_out_i = pack_signed(bin.i, kCarrierWidth);
        top.fft_core_out_q = pack_signed(bin.q, kCarrierWidth);
        top.fft_core_out_block_exp = 0;
        top.fft_core_out_overflow = 0;
    }
#endif

    if (!cfg.internal_data_scheduler && scheduler.command_valid()) {
        top.data_symbol_cmd_valid = 1;
        top.data_symbol_cmd_fft_start_sample = scheduler.command_fft_start();
    }
}

CycleFires observe_cycle(const TopType &top) {
    CycleFires fires;

    fires.sdr = top.sdr_in_valid && top.sdr_in_ready;

#if !defined(DAB_TB_CUSTOM_FFT)
    fires.fft_in = top.fft_core_in_valid && top.fft_core_in_ready;
    fires.fft_in_last = top.fft_core_in_last != 0;
    fires.fft_in_i = static_cast<int32_t>(sign_extend(top.fft_core_in_i, 18));
    fires.fft_in_q = static_cast<int32_t>(sign_extend(top.fft_core_in_q, 18));

    fires.fft_out = top.fft_core_out_valid && top.fft_core_out_ready;
#endif
    fires.data_cmd = top.data_symbol_cmd_valid && top.data_symbol_cmd_ready;

    fires.sync = top.sync_out_valid && top.sync_out_ready;
    fires.sync_found = top.sync_out_found != 0;
    fires.sync_locked = top.sync_out_locked != 0;
    fires.sync_prs_fft_start = top.sync_out_prs_fft_start_sample;
    fires.sync_prs_score = top.sync_out_prs_score;
    fires.sync_pilot_score = top.sync_out_pilot_score;
    fires.sync_carrier_offset = static_cast<int32_t>(sign_extend(top.sync_out_carrier_offset_bins, 4));
    fires.sync_fine_timing = static_cast<int32_t>(sign_extend(top.sync_out_fine_timing_error_samples, 24));

    fires.softbit = top.tframe_out_valid && top.tframe_out_ready;
    fires.softbit_value = static_cast<int8_t>(sign_extend(top.tframe_out_softbit, 8));
    fires.frame_done = fires.softbit && top.tframe_out_frame_done;
    fires.frame_start_sample = top.tframe_out_frame_start_sample;
    return fires;
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    try {
        const Config cfg = load_config();
        const std::vector<Sample> samples = read_iq_f32_file(cfg);

        std::ofstream softbits_out(cfg.softbits_path.c_str(),
                                   std::ios::binary | std::ios::trunc);
        if (!softbits_out) {
            throw std::runtime_error("failed to open softbit output: " + cfg.softbits_path);
        }

        TopType top;
#if !defined(DAB_TB_CUSTOM_FFT)
        FftShim fft(cfg);
#endif
        DataScheduler scheduler;

        reset_top(top, cfg);

        uint64_t sample_pos = 0;
        uint64_t softbit_count = 0;
        uint64_t sync_events = 0;
        uint64_t found_sync_events = 0;
        bool feed_enabled = true;
        bool feed_paused_for_frame = false;
        uint32_t frames_done = 0;
        uint64_t last_frame_start_sample = 0;

        std::cout << "dab_pipeline_file start iq_path=" << cfg.iq_path
                  << " samples=" << samples.size()
                  << " softbits_path=" << cfg.softbits_path
                  << " target_tframes=" << cfg.target_tframes
                  << " require_frame=" << (cfg.require_frame ? 1 : 0)
                  << " data_scheduler="
                  << (cfg.internal_data_scheduler ? "internal" : "external")
                  << " cp_search=" << (cfg.streaming_cp_search ? "stream" : "history")
                  << " sample_period_cycles=" << cfg.sample_period_cycles
#if defined(DAB_TB_CUSTOM_FFT)
                  << " fft_impl=custom_rtl"
#else
                  << " fft_impl=cpp_shim"
                  << " fft_scale=" << (cfg.fft_fixed_gain ? "fixed" : "rms")
#endif
                  << "\n";

        for (uint64_t cycle = 0; cycle < cfg.max_cycles && !Verilated::gotFinish(); ++cycle) {
            if (!cfg.internal_data_scheduler && scheduler.can_arm(top)) {
                scheduler.arm();
                if (cfg.stop_feed_after_frame_ready) {
                    feed_enabled = false;
                } else if (cfg.target_tframes > 1u) {
                    feed_enabled = false;
                    feed_paused_for_frame = true;
                }
                std::cout << "scheduler armed prs_fft_start=" << scheduler.prs_fft_start()
                          << " frame_required_end=" << scheduler.frame_required_end_sample()
                          << " cycle=" << cycle << "\n";
            }

            const bool sample_cycle =
                (cycle % static_cast<uint64_t>(cfg.sample_period_cycles)) == 0u;
            drive_cycle_inputs(top,
                               cfg,
#if !defined(DAB_TB_CUSTOM_FFT)
                               fft,
#endif
                               scheduler,
                               samples,
                               sample_pos,
                               feed_enabled,
                               sample_cycle);

            top.clk = 0;
            top.eval();
            const CycleFires fires = observe_cycle(top);
            top.clk = 1;
            top.eval();

            if (fires.sdr) {
                ++sample_pos;
            }
#if !defined(DAB_TB_CUSTOM_FFT)
            if (fires.fft_in) {
                fft.push_input(fires.fft_in_i, fires.fft_in_q, fires.fft_in_last);
            }
            if (fires.fft_out) {
                fft.pop_output();
            }
#endif
            if (!cfg.internal_data_scheduler && fires.data_cmd) {
                scheduler.command_accepted();
            }
            if (fires.sync) {
                ++sync_events;
                if (fires.sync_found) {
                    ++found_sync_events;
                    std::cout << "sync found prs_fft_start=" << fires.sync_prs_fft_start
                              << " prs_score=" << score_to_float(fires.sync_prs_score)
                              << " pilot_score=" << score_to_float(fires.sync_pilot_score)
                              << " locked=" << (fires.sync_locked ? 1 : 0)
                              << " ifo=" << fires.sync_carrier_offset
                              << " fine_timing_q12=" << fires.sync_fine_timing
                              << " cycle=" << cycle << "\n";
                }
                (void)scheduler.observe_sync(fires);
            }
            if (fires.softbit) {
                softbits_out.write(reinterpret_cast<const char *>(&fires.softbit_value),
                                   sizeof(fires.softbit_value));
                if (!softbits_out) {
                    throw std::runtime_error("failed while writing softbit output");
                }
                ++softbit_count;
            }
            if (fires.frame_done) {
                ++frames_done;
                if (!cfg.internal_data_scheduler) {
                    scheduler.frame_done();
                }
                last_frame_start_sample = fires.frame_start_sample;
                std::cout << "frame done index=" << frames_done
                          << " frame_start_sample=" << last_frame_start_sample
                          << " softbits=" << softbit_count
                          << " cycle=" << cycle << "\n";
                if (frames_done >= cfg.target_tframes) {
                    break;
                }
                if (!cfg.internal_data_scheduler && feed_paused_for_frame &&
                    sample_pos < samples.size()) {
                    feed_enabled = true;
                    feed_paused_for_frame = false;
                }
            }

            if (sample_pos >= samples.size()) {
                feed_enabled = false;
            }

            if (cfg.log_every_cycles != 0 && cycle != 0 && (cycle % cfg.log_every_cycles) == 0) {
                std::cout << "progress cycle=" << cycle
                          << " samples_in=" << sample_pos
                          << " history_next=" << top.history_next_sample_index
                          << " sync_events=" << sync_events
                          << " found=" << found_sync_events
                          << " scheduler_armed=" << (scheduler.armed() ? 1 : 0)
                          << " scheduler_waiting=" << (scheduler.waiting_for_frame_done() ? 1 : 0)
                          << " scheduler_pending=" << scheduler.pending_count()
                          << " data_next=" << scheduler.next_symbol()
                          << " frames_done=" << frames_done
                          << " softbits=" << softbit_count
#if !defined(DAB_TB_CUSTOM_FFT)
                          << " ffts=" << fft.fft_count()
#endif
                          << "\n";
            }
        }

        softbits_out.close();
        if (!softbits_out) {
            throw std::runtime_error("failed to close softbit output");
        }

        std::cout << "dab_pipeline_file done samples_in=" << sample_pos
                  << " sync_events=" << sync_events
                  << " found_sync_events=" << found_sync_events
                  << " sync_frames_observed=" << scheduler.observed_frames()
                  << " data_frames_scheduled="
                  << (cfg.internal_data_scheduler ? uint64_t{frames_done} :
                                                     scheduler.scheduled_frames())
                  << " last_data_symbol_next=" << scheduler.next_symbol()
                  << " frames_done=" << frames_done
                  << " last_frame_start_sample=" << last_frame_start_sample
                  << " softbits=" << softbit_count
#if !defined(DAB_TB_CUSTOM_FFT)
                  << " ffts=" << fft.fft_count()
                  << " fft_last_scale=" << fft.last_scale()
                  << " fft_max_raw_bin_abs=" << fft.max_raw_bin_abs()
                  << " fft_saturations=" << fft.saturation_count()
#else
                  << " fft_impl=custom_rtl"
#endif
                  << "\n";

        if (frames_done >= cfg.target_tframes) {
            const uint64_t expected_softbits = uint64_t{cfg.target_tframes} * kFrameSoftbits;
            if (softbit_count != expected_softbits) {
                std::cerr << "frame completed with unexpected softbit count expected="
                          << expected_softbits << " actual=" << softbit_count << "\n";
                return 1;
            }
            return 0;
        }

        if (cfg.require_frame) {
            std::cerr << "only produced " << frames_done << " complete frame(s), target="
                      << cfg.target_tframes << ", before timeout or EOF\n";
            return 1;
        }
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "tb_dab_pipeline_file error: " << ex.what() << "\n";
        return 1;
    }
}
