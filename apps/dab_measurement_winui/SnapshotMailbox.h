#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

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
        bool ReadSince(uint64_t previous_version,
                       std::vector<dab_receive_snapshot_t>& snapshots,
                       uint64_t& version) const;

    private:
        struct HistoryEntry
        {
            uint64_t version;
            dab_receive_snapshot_t snapshot;
        };

        static constexpr size_t history_capacity = 128u;
        mutable std::mutex m_mutex;
        dab_receive_snapshot_t m_latest{};
        std::deque<HistoryEntry> m_history;
        uint64_t m_version{ 0 };
        bool m_valid{ false };
    };
}
