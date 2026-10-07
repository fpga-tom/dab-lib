#include "pch.h"
#include "SnapshotMailbox.h"

namespace dab::measurement
{
    int SnapshotMailbox::Callback(dab_receive_snapshot_t const* snapshot, void* user) noexcept
    {
        if (snapshot == nullptr || user == nullptr)
        {
            return -1;
        }

        static_cast<SnapshotMailbox*>(user)->Publish(*snapshot);
        return 0;
    }

    void SnapshotMailbox::Publish(dab_receive_snapshot_t const& snapshot) noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_latest = snapshot;
        ++m_version;
        m_history.push_back(HistoryEntry{ m_version, snapshot });
        while (m_history.size() > history_capacity)
        {
            m_history.pop_front();
        }
        m_valid = true;
    }

    bool SnapshotMailbox::TryReadLatest(uint64_t previous_version,
                                        dab_receive_snapshot_t& snapshot,
                                        uint64_t& version) const noexcept
    {
        std::scoped_lock lock(m_mutex);
        if (!m_valid || m_version == previous_version)
        {
            return false;
        }

        snapshot = m_latest;
        version = m_version;
        return true;
    }

    bool SnapshotMailbox::ReadSince(uint64_t previous_version,
                                    std::vector<dab_receive_snapshot_t>& snapshots,
                                    uint64_t& version) const
    {
        std::scoped_lock lock(m_mutex);
        snapshots.clear();
        if (!m_valid || m_version == previous_version)
        {
            return false;
        }

        for (auto const& entry : m_history)
        {
            if (entry.version > previous_version)
            {
                snapshots.push_back(entry.snapshot);
            }
        }
        version = m_version;
        return !snapshots.empty();
    }
}
