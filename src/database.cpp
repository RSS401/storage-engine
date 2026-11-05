#include<rohandb/database.hpp>

namespace rohandb
{
    Database::Database() = default;

    void Database::set(const std::string& key, const std::string& value)
    {
        table_[key] = value;
    }

    std::optional<std::string> Database::get(const std::string& key) const {
        auto it = table_.find(key);
        if (it == table_.end()) return std::nullopt;
        return it->second;
    }

    bool Database::remove(const std::string& key) {
        auto it = table_.find(key);
        if (it == table_.end()) return false;
        table_.erase(it);
        return true;
    }

    bool Database::contains(const std::string& key) const {
        return table_.find(key) != table_.end();
    }
}