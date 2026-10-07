#include "pch.h"
#include "CatalogMailbox.h"

namespace dab::measurement
{
    int CatalogMailbox::Callback(dab_receive_catalog_t const* catalog, void* user) noexcept
    {
        if (catalog == nullptr || user == nullptr)
        {
            return -1;
        }

        static_cast<CatalogMailbox*>(user)->Publish(*catalog);
        return 0;
    }

    void CatalogMailbox::Publish(dab_receive_catalog_t const& catalog) noexcept
    {
        std::scoped_lock lock(m_mutex);
        m_latest = catalog;
        ++m_version;
        m_valid = true;
    }

    bool CatalogMailbox::TryReadLatest(uint64_t previous_version,
                                       dab_receive_catalog_t& catalog,
                                       uint64_t& version) const noexcept
    {
        std::scoped_lock lock(m_mutex);
        if (!m_valid || m_version == previous_version)
        {
            return false;
        }

        catalog = m_latest;
        version = m_version;
        return true;
    }
}
