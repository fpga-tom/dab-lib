#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "Vqpsk_softbits.h"
#include "verilated.h"

namespace {

constexpr uint32_t kDataCarriers = 1536;
constexpr uint32_t kCarrierMask = (1u << 24u) - 1u;

int32_t sign_extend_24(uint32_t value) {
    value &= kCarrierMask;
    if ((value & (1u << 23u)) != 0u) {
        value |= ~kCarrierMask;
    }
    return static_cast<int32_t>(value);
}

uint32_t pack24(int32_t value) {
    return static_cast<uint32_t>(value) & kCarrierMask;
}

int32_t carrier_i(uint32_t index, uint32_t seed) {
    switch (index % 257u) {
        case 0u: return 0;
        case 1u: return (1 << 23) - 1;
        case 2u: return -(1 << 23);
        default:
            return sign_extend_24(((index * 14563u) ^ (seed * 101111u) ^ 0x00432110u) & kCarrierMask);
    }
}

int32_t carrier_q(uint32_t index, uint32_t seed) {
    switch (index % 257u) {
        case 0u: return 0;
        case 1u: return 0;
        case 2u: return 0;
        default:
            return sign_extend_24(((index * 7219u) ^ (seed * 202333u) ^ 0x00cdef44u) & kCarrierMask);
    }
}

uint32_t isqrt_u64(uint64_t value) {
    uint64_t op = value;
    uint64_t res = 0;
    uint64_t one = 1ull << 62;

    while (one > op) {
        one >>= 2;
    }
    while (one != 0u) {
        if (op >= res + one) {
            op -= res + one;
            res = (res >> 1u) + one;
        } else {
            res >>= 1u;
        }
        one >>= 2u;
    }

    return static_cast<uint32_t>(res);
}

uint32_t magnitude(int32_t i, int32_t q) {
    const uint64_t abs_i = i < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(i))
                                 : static_cast<uint64_t>(i);
    const uint64_t abs_q = q < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(q))
                                 : static_cast<uint64_t>(q);
    return isqrt_u64((abs_i * abs_i) + (abs_q * abs_q));
}

int8_t softbit(int32_t component, uint32_t mag) {
    int64_t quotient = 0;

    if (mag != 0u) {
        quotient = (-static_cast<int64_t>(component) * 255ll) / (static_cast<int64_t>(mag) * 2ll);
    }
    if (quotient > 127) {
        return 127;
    }
    if (quotient < -128) {
        return -128;
    }
    return static_cast<int8_t>(quotient);
}

void tick(Vqpsk_softbits &top) {
    top.clk = 0;
    top.eval();
    top.clk = 1;
    top.eval();
}

void reset(Vqpsk_softbits &top) {
    top.rst_n = 0;
    top.clear = 0;
    top.in_valid = 0;
    top.out_ready = 0;
    top.in_i = 0;
    top.in_q = 0;
    top.in_carrier_index = 0;
    top.in_symbol_start_sample = 0;
    top.in_symbol_done = 0;
    tick(top);
    tick(top);
    top.rst_n = 1;
    tick(top);
}

void fail_at(const char *field, uint32_t index, int64_t expected, int64_t actual) {
    std::cerr << "Mismatch at output " << index << " field=" << field
              << " expected=" << expected << " actual=" << actual << "\n";
    std::exit(1);
}

void check_output(const Vqpsk_softbits &top,
                  uint32_t index,
                  uint64_t symbol_start_sample,
                  uint32_t seed) {
    const int32_t i = carrier_i(index, seed);
    const int32_t q = carrier_q(index, seed);
    const uint32_t mag = magnitude(i, q);
    const int8_t expected_a = softbit(i, mag);
    const int8_t expected_b = softbit(q, mag);

    if (static_cast<int8_t>(top.out_soft_a) != expected_a) {
        fail_at("soft_a", index, expected_a, static_cast<int8_t>(top.out_soft_a));
    }
    if (static_cast<int8_t>(top.out_soft_b) != expected_b) {
        fail_at("soft_b", index, expected_b, static_cast<int8_t>(top.out_soft_b));
    }
    if (top.out_carrier_index != index) {
        fail_at("carrier_index", index, index, top.out_carrier_index);
    }
    if (top.out_symbol_start_sample != symbol_start_sample) {
        fail_at("symbol_start_sample", index, symbol_start_sample, top.out_symbol_start_sample);
    }
    if (top.out_symbol_done != (index == kDataCarriers - 1u ? 1u : 0u)) {
        fail_at("symbol_done",
                index,
                index == kDataCarriers - 1u ? 1u : 0u,
                top.out_symbol_done);
    }
}

void run_symbol(Vqpsk_softbits &top, uint64_t symbol_start_sample, uint32_t seed) {
    uint32_t sent = 0;
    uint32_t received = 0;
    uint32_t cycles = 0;

    while (received < kDataCarriers) {
        top.out_ready = ((cycles % 9u) != 5u) ? 1 : 0;
        top.in_valid = sent < kDataCarriers ? 1 : 0;
        top.in_i = sent < kDataCarriers ? pack24(carrier_i(sent, seed)) : 0;
        top.in_q = sent < kDataCarriers ? pack24(carrier_q(sent, seed)) : 0;
        top.in_carrier_index = sent;
        top.in_symbol_start_sample = symbol_start_sample;
        top.in_symbol_done = sent == kDataCarriers - 1u ? 1 : 0;
        top.eval();

        const bool output_fire = top.out_valid && top.out_ready;
        const bool input_fire = top.in_valid && top.in_ready;

        if (output_fire) {
            check_output(top, received, symbol_start_sample, seed);
            ++received;
        }

        tick(top);

        if (input_fire) {
            ++sent;
        }

        ++cycles;
        if (cycles > kDataCarriers * 4u) {
            std::cerr << "Timeout sent=" << sent << " received=" << received << "\n";
            std::exit(1);
        }
    }

    top.out_ready = 1;
    top.in_valid = 0;
    tick(top);
    top.eval();
    if (top.out_valid) {
        std::cerr << "Output valid remained asserted after symbol drain\n";
        std::exit(1);
    }
}

void run_clear_test(Vqpsk_softbits &top) {
    top.out_ready = 1;
    top.in_valid = 1;
    top.in_i = pack24(carrier_i(3, 77));
    top.in_q = pack24(carrier_q(3, 77));
    top.in_carrier_index = 3;
    top.in_symbol_start_sample = 44;
    top.in_symbol_done = 0;
    top.eval();
    tick(top);

    top.clear = 1;
    top.in_valid = 0;
    tick(top);
    top.clear = 0;
    top.eval();
    if (top.out_valid || !top.in_ready) {
        std::cerr << "Clear did not return qpsk_softbits to idle state\n";
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);

    Vqpsk_softbits top;

    reset(top);
    run_symbol(top, 12345ull, 1);
    run_clear_test(top);
    run_symbol(top, 67890ull, 11);

    std::cout << "qpsk_softbits: PASS\n";
    return 0;
}
