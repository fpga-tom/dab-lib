#pragma once

#include <cstdint>
#include <mutex>

#include "dab/receive.h"

namespace dab::measurement
{
    class SnapshotMailbox
    {
    public:
        static int Callback(dab_receive_snapshot_t const* snapshot, void* user) noexcept;

        void Publish(dab_receive_snapshot_t const& snapshot) noexcept;
        bool TryReadLatest(uint64_t previous_version,
                           dab_receive_snapshot_t& snapshot,
                           uint64_t& version) const noexcept;

    private:
        mutable std::mutex m_mutex;
        dab_receive_snapshot_t m_latest{};
        uint64_t m_version{ 0 };
        bool m_valid{ false };
    };
}
