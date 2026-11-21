#include "output.hpp"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>

void require(arrow::Status status) {
    if (!status.ok()) throw std::runtime_error(status.ToString());
}

template<class Value>
Value require(arrow::Result<Value> result) {
    if (!result.ok()) throw std::runtime_error(result.status().ToString());
    return std::move(result).ValueUnsafe();
}

void write_parquet(const fs::path& path, const std::shared_ptr<arrow::Schema>& schema,
                   std::vector<std::shared_ptr<arrow::Array>> arrays) {
    auto file = require(arrow::io::FileOutputStream::Open(path.string()));
    parquet::WriterProperties::Builder properties;
    properties.compression(parquet::Compression::SNAPPY);
    auto table = arrow::Table::Make(schema, std::move(arrays));
    require(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), file, 65536,
                                       properties.build()));
    require(file->Close());
}

template<class... Builders>
auto finish(Builders&... builders) {
    return std::vector<std::shared_ptr<arrow::Array>>{require(builders.Finish())...};
}

template<class... Builders>
void reserve(size_t rows, Builders&... builders) {
    (require(builders.Reserve(rows)), ...);
}

void write_users(const fs::path& path, const std::vector<std::vector<UserDay>>& partitions,
                 size_t start, size_t end, const std::vector<Text>& categories) {
    arrow::StringBuilder person, top;
    arrow::Date32Builder day;
    arrow::Int64Builder sessions, events, revenue;
    size_t rows = 0;
    for (size_t i = start; i < end; ++i) rows += partitions[i].size();
    reserve(rows, person, top, day, sessions, events, revenue);
    for (size_t i = start; i < end; ++i) {
        for (const auto& row : partitions[i]) {
            std::array<char, 9> name;
            require(person.Append(row.person.text(name)));
            if (row.category == no_category) require(top.AppendNull());
            else require(top.Append(categories[row.category]));
            day.UnsafeAppend(row.day);
            sessions.UnsafeAppend(row.sessions);
            events.UnsafeAppend(row.events);
            revenue.UnsafeAppend(row.revenue);
        }
    }
    const auto schema = arrow::schema({
        arrow::field("user_id", arrow::utf8(), false), arrow::field("day", arrow::date32(), false),
        arrow::field("sessions", arrow::int64(), false), arrow::field("events", arrow::int64(), false),
        arrow::field("revenue_cents", arrow::int64(), false), arrow::field("top_category", arrow::utf8())
    });
    write_parquet(path, schema, finish(person, day, sessions, events, revenue, top));
}

void write_campaigns(const fs::path& path, const CampaignDays& rows,
                     const std::vector<Campaign>& campaigns) {
    arrow::StringBuilder id, channel;
    arrow::Date32Builder day;
    arrow::Int64Builder sessions, purchasing_sessions, revenue, assisted_sessions;
    reserve(rows.size(), id, channel, day, sessions, purchasing_sessions, revenue, assisted_sessions);
    for (const auto& [key, row] : rows) {
        const auto& campaign = campaigns[key >> 32];
        require(id.Append(campaign.id));
        require(channel.Append(campaign.channel));
        day.UnsafeAppend(static_cast<int32_t>(key));
        sessions.UnsafeAppend(row.sessions);
        purchasing_sessions.UnsafeAppend(row.purchasing_sessions);
        revenue.UnsafeAppend(row.revenue);
        assisted_sessions.UnsafeAppend(row.assisted_sessions);
    }
    const auto schema = arrow::schema({
        arrow::field("campaign_id", arrow::utf8(), false), arrow::field("channel", arrow::utf8(), false),
        arrow::field("day", arrow::date32(), false), arrow::field("sessions", arrow::int64(), false),
        arrow::field("purchasing_sessions", arrow::int64(), false),
        arrow::field("revenue_cents", arrow::int64(), false),
        arrow::field("assisted_sessions", arrow::int64(), false)
    });
    write_parquet(path, schema, finish(id, channel, day, sessions, purchasing_sessions,
                                       revenue, assisted_sessions));
}
