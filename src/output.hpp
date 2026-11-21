#pragma once

#include "key.hpp"
#include <unordered_map>

struct UserDay {
    Key person;
    int32_t day;
    uint32_t category;
    int64_t sessions;
    int64_t events;
    int64_t revenue;
};

struct CampaignDay {
    int64_t sessions = 0;
    int64_t purchasing_sessions = 0;
    int64_t revenue = 0;
    int64_t assisted_sessions = 0;

    void add(const CampaignDay& other) {
        sessions += other.sessions;
        purchasing_sessions += other.purchasing_sessions;
        revenue += other.revenue;
        assisted_sessions += other.assisted_sessions;
    }
};

struct Campaign {
    Text id;
    Text channel;
};

constexpr uint32_t no_category = std::numeric_limits<uint32_t>::max();
using CampaignDays = std::unordered_map<uint64_t, CampaignDay>;

inline uint64_t campaign_day_key(uint32_t campaign, int32_t day) {
    return (uint64_t{campaign} << 32) | static_cast<uint32_t>(day);
}

void write_users(const fs::path& path, const std::vector<std::vector<UserDay>>& partitions,
                 size_t start, size_t end, const std::vector<Text>& categories);
void write_campaigns(const fs::path& path, const CampaignDays& rows,
                     const std::vector<Campaign>& campaigns);
