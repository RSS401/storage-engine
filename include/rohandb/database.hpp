#pragma once
#include <string>
#include <optional>
#include <unordered_map>
namespace rohandb
{
    class Database
    {
    public:
        Database();

        void set(const std::string& key, const std::string& value);

        std::optional<std::string> get(const std::string& key) const;

        bool remove(const std::string& key);

        bool contains(const std::string& key) const;

    private:
        std::unordered_map<std::string, std::string> table_;
    };

    
}