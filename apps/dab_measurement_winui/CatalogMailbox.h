#pragma once

#include <cstdint>
#include <mutex>

#include "dab/receive.h"

namespace dab::measurement
{
    class CatalogMailbox
    {
    public:
        static int Callback(dab_receive_catalog_t const* catalog, void* user) noexcept;

        void Publish(dab_receive_catalog_t const& catalog) noexcept;
        bool TryReadLatest(uint64_t previous_version,
                           dab_receive_catalog_t& catalog,
                           uint64_t& version) const noexcept;

    private:
        mutable std::mutex m_mutex;
        dab_receive_catalog_t m_latest{};
        uint64_t m_version{ 0 };
        bool m_valid{ false };
    };
}
