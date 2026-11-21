#include "input.hpp"
#include "output.hpp"
#include "key.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <iostream>
#include <future>
#include <optional>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>

constexpr size_t event_shards = 256;
constexpr size_t person_shards = 64;
constexpr uint32_t no_index = std::numeric_limits<uint32_t>::max();

template<class Function>
void parallel(size_t items, size_t workers, Function run) {
    if (!items) return;
    std::atomic<size_t> next{0};
    std::exception_ptr failure;
    std::mutex failure_lock;
    auto work = [&](size_t worker) {
        try {
            for (size_t index; (index = next.fetch_add(1, std::memory_order_relaxed)) < items;) {
                run(worker, index);
            }
        } catch (...) {
            next.store(items, std::memory_order_relaxed);
            const std::lock_guard lock(failure_lock);
            if (!failure) failure = std::current_exception();
        }
    };
    std::vector<std::jthread> threads;
    for (size_t i = 1; i < std::min(workers, items); ++i) threads.emplace_back(work, i);
    work(0);
    threads.clear();
    if (failure) std::rethrow_exception(failure);
}

class Timings {
public:
    Timings() : begin_(Clock::now()), last_(begin_) {}
    void mark(const char* name) {
        const auto now = Clock::now();
        std::cerr << name << "_s=" << std::chrono::duration<double>(now - last_).count() << '\n';
        last_ = now;
    }
    void total() const {
        std::cerr << "total_s=" << std::chrono::duration<double>(Clock::now() - begin_).count() << '\n';
    }
private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point begin_, last_;
};

using Index = std::unordered_map<Text, uint32_t>;

class TextIndex {
public:
    explicit TextIndex(size_t expected = 0) {
        size_t capacity = 16;
        while (capacity - capacity / 4 < expected) capacity *= 2;
        slots_.resize(capacity);
    }

    uint32_t lookup(Key key) const {
        const size_t hash = key.hash();
        return slots_[position(key, hash)].index;
    }

    uint32_t intern(Key key, uint32_t index) {
        const size_t hash = key.hash();
        size_t at = position(key, hash);
        if (slots_[at].index != no_index) return slots_[at].index;
        if (size_ == slots_.size() - slots_.size() / 4) {
            auto old = std::exchange(slots_, {});
            slots_.resize(old.size() * 2);
            for (const auto& slot : old) {
                if (slot.index != no_index) slots_[position(slot.key, slot.hash)] = slot;
            }
            at = position(key, hash);
        }
        slots_[at] = {key, static_cast<uint32_t>(hash), index};
        ++size_;
        return index;
    }

private:
    struct Slot {
        Key key;
        uint32_t hash = 0, index = no_index;
    };

    size_t position(Key key, uint32_t hash) const {
        const size_t mask = slots_.size() - 1;
        size_t at = std::rotr(hash, 6) & mask;
        while (slots_[at].index != no_index && (slots_[at].hash != hash || slots_[at].key != key)) {
            at = (at + 1) & mask;
        }
        return at;
    }

    std::vector<Slot> slots_;
    size_t size_ = 0;
};

class Identities {
public:
    explicit Identities(const InputParts& parts)
        : indexes_(std::min<size_t>(parts.bytes / 16, 1 << 22)) {
        std::vector<Key> ids;
        const auto intern = [&](Key id) {
            const uint32_t next = parent_.size();
            const auto index = indexes_.intern(id, next);
            if (index == next) {
                parent_.push_back(next);
                size_.push_back(1);
                ids.push_back(id);
            }
            return index;
        };
        for (Text chunk : parts.chunks) {
            for_lines(chunk, [&](Text line) {
                std::array<Text, 2> fields;
                if (split_csv(line, fields) && !fields[0].empty() && !fields[1].empty()) {
                    const auto a = intern(keys_.make(fields[0]));
                    const auto b = intern(keys_.make(fields[1]));
                    join(a, b);
                }
            });
        }
        names_.resize(parent_.size());
        for (size_t index = 0; index < ids.size(); ++index) {
            const Key name = ids[index];
            const auto root = find(index);
            if (names_[root].empty() || name < names_[root]) names_[root] = name;
        }
        for (size_t i = 0; i < parent_.size(); ++i) names_[i] = names_[find(i)];
        std::vector<uint32_t>().swap(parent_);
        std::vector<uint32_t>().swap(size_);
    }

    Key person(Key id) const {
        const auto found = indexes_.lookup(id);
        return found == no_index ? id : names_[found];
    }

private:
    uint32_t find(uint32_t node) {
        while (parent_[node] != node) {
            parent_[node] = parent_[parent_[node]];
            node = parent_[node];
        }
        return node;
    }

    void join(uint32_t a, uint32_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (size_[a] < size_[b]) std::swap(a, b);
        parent_[b] = a;
        size_[a] += size_[b];
    }

    KeyArena keys_;
    TextIndex indexes_;
    std::vector<uint32_t> parent_, size_;
    std::vector<Key> names_;
};

struct Lookups {
    Mapping product_file, campaign_file;
    Index products, campaign_index;
    std::vector<Text> categories{"unknown"};
    std::vector<Campaign> campaigns{{"organic", "organic"}};

    explicit Lookups(const fs::path& data)
        : product_file(data / "products.csv"), campaign_file(data / "campaigns.csv") {
        Index category_index;
        for_lines(after_header(product_file.data()), [&](Text line) {
            std::array<Text, 2> fields;
            if (!split_csv(line, fields)) throw std::runtime_error("Malformed product lookup");
            const auto [category, inserted] = category_index.try_emplace(fields[1], categories.size());
            if (inserted) categories.push_back(fields[1]);
            products.emplace(fields[0], category->second);
        });
        for_lines(after_header(campaign_file.data()), [&](Text line) {
            std::array<Text, 2> fields;
            if (!split_csv(line, fields)) throw std::runtime_error("Malformed campaign lookup");
            campaign_index.emplace(fields[0], campaigns.size());
            campaigns.push_back({fields[0], fields[1]});
        });
    }
};

struct Event {
    Key person;
    Time time;
    uint32_t price, category;
};

struct EventCopy {
    Key id;
    Time ingest;
    Event event;
};

struct Click {
    Key person, id;
    Time time;
    uint32_t campaign;
};

class EventCopyBuffer {
public:
    size_t size() const { return size_; }

    void push_back(const EventCopy& copy) {
        const size_t offset = size_ % block_size;
        if (!offset) blocks_.push_back(std::make_unique_for_overwrite<EventCopy[]>(block_size));
        blocks_.back()[offset] = copy;
        ++size_;
    }

    template<class Function>
    void visit(Function use) {
        for (size_t block = 0; block < blocks_.size(); ++block) {
            const size_t count = std::min(block_size, size_ - block * block_size);
            for (size_t offset = 0; offset < count; ++offset) use(blocks_[block][offset]);
        }
    }

    void clear() {
        blocks_.clear();
        size_ = 0;
    }

private:
    static constexpr size_t block_size = 1024;
    std::vector<std::unique_ptr<EventCopy[]>> blocks_;
    size_t size_ = 0;
};

using EventCopies = std::array<EventCopyBuffer, event_shards>;
using Events = std::array<std::vector<Event>, person_shards>;
using Clicks = std::array<std::vector<Click>, person_shards>;

Event validate_event(const std::array<Text, 7>& row, Time ingest, const Window& window,
                     const Lookups& lookups, KeyArena& keys, TimestampParser& times) {
    const auto type = row[4];
    if (row[1].empty() || row[5].empty() || (type != "view" && type != "cart" && type != "purchase")) {
        return {};
    }
    const Time time = times.parse(row[2]);
    if (time == invalid_time || time < window.begin || time >= window.end || ingest - time > day_ms) {
        return {};
    }
    uint32_t price = 0;
    uint32_t category = 0;
    if (type == "purchase") {
        const Time parsed = unsigned_decimal(row[6], 10000000);
        if (parsed <= 0) return {};
        price = parsed;
        const auto product = lookups.products.find(row[5]);
        if (product != lookups.products.end()) category = product->second;
    }
    return {keys.make(row[1]), time, price, category};
}

void parse_events(Text chunk, EventCopies& shards, const Window& window,
                  const Lookups& lookups, KeyArena& keys) {
    TimestampParser times;
    for_csv<7>(chunk, [&](const auto& row) {
        if (row[0].empty()) return;
        const Time ingest = times.ingest(row[3]);
        if (ingest == invalid_time) throw std::runtime_error("Invalid ingest_ts");
        const Key id = keys.make(row[0]);
        const auto shard = id.hash() % event_shards;
        // A parseable but invalid copy must still be eligible to win deduplication.
        shards[shard].push_back({id, ingest, validate_event(row, ingest, window, lookups, keys, times)});
    });
}

void deduplicate(std::vector<EventCopies>& copies, size_t shard, Events& people,
                 const Identities& identities) {
    size_t size = 0;
    for (const auto& worker : copies) size += worker[shard].size();
    if (!size) return;

    // Keep the map local to a shard. Hashes choose a slot, and full IDs establish equality.
    size_t capacity = 1;
    while (capacity < size + size / 2 + 1) capacity *= 2;
    const size_t mask = capacity - 1;
    std::vector<EventCopy*> slots(capacity);
    for (auto& worker : copies) {
        worker[shard].visit([&](auto& row) {
            size_t at = (row.id.hash() / event_shards) & mask;
            while (slots[at] && slots[at]->id != row.id) {
                at = (at + 1) & mask;
            }
            auto*& kept = slots[at];
            if (!kept || row.ingest < kept->ingest) {
                if (kept) kept->event.person = Key{};
                kept = &row;
            } else {
                row.event.person = Key{};
            }
        });
    }
    for (auto& worker : copies) {
        worker[shard].visit([&](const auto& row) {
            if (!row.event.person.empty()) {
                Event event = row.event;
                event.person = identities.person(event.person);
                people[event.person.hash() % person_shards].push_back(event);
            }
        });
        worker[shard].clear();
    }
}

void parse_clicks(Text chunk, Clicks& shards, const Window& window,
                  const Lookups& lookups, const Identities& identities, KeyArena& keys) {
    TimestampParser times;
    for_csv<4>(chunk, [&](const auto& row) {
        if (row[0].empty() || row[1].empty()) return;
        const Time time = times.parse(row[3]);
        if (time == invalid_time || time < window.begin - week_ms || time >= window.end) return;
        const auto campaign = lookups.campaign_index.find(row[2]);
        if (campaign == lookups.campaign_index.end()) return;
        const Key person = identities.person(keys.make(row[1]));
        shards[person.hash() % person_shards].push_back({person, keys.make(row[0]), time, campaign->second});
    });
}

struct IndexedEvent {
    Time time;
    uint32_t person, price, category;
};

struct IndexedClick {
    Time time;
    uint32_t person, campaign;
    Key id;
};

struct Session {
    Time start;
    int64_t revenue;
    uint32_t person;
};

struct Partition {
    std::vector<IndexedEvent> events;
    std::vector<IndexedClick> clicks;
    std::vector<Key> names;
    std::vector<Session> sessions;
};

void index_and_sort(size_t shard, std::vector<Events>& event_workers,
                    std::vector<Clicks>& click_workers, Partition& partition) {
    size_t events = 0, clicks = 0;
    for (const auto& worker : event_workers) events += worker[shard].size();
    for (const auto& worker : click_workers) clicks += worker[shard].size();
    partition.events.reserve(events);
    partition.clicks.reserve(clicks);
    TextIndex people(std::min<size_t>(events / 24, 1 << 18));
    std::vector<size_t> counts;
    for (auto& worker : event_workers) {
        for (const auto& event : worker[shard]) {
            const uint32_t next = partition.names.size();
            const auto index = people.intern(event.person, next);
            if (index == next) {
                partition.names.push_back(event.person);
                counts.push_back(0);
            }
            ++counts[index];
            partition.events.push_back({event.time, index, event.price, event.category});
        }
        std::vector<Event>().swap(worker[shard]);
    }
    for (auto& worker : click_workers) {
        for (const auto& click : worker[shard]) {
            const auto person = people.lookup(click.person);
            if (person != no_index) {
                partition.clicks.push_back({click.time, person, click.campaign, click.id});
            }
        }
        std::vector<Click>().swap(worker[shard]);
    }
    // Group by compact person index with prefix sums, then sort timestamps within each person.
    std::vector<size_t> offsets(counts.size() + 1);
    std::partial_sum(counts.begin(), counts.end(), offsets.begin() + 1);
    auto positions = offsets;
    std::vector<IndexedEvent> grouped(events);
    for (const auto& event : partition.events) grouped[positions[event.person]++] = event;
    for (size_t person = 0; person < counts.size(); ++person) {
        std::sort(grouped.begin() + offsets[person], grouped.begin() + offsets[person + 1],
                  [](const auto& a, const auto& b) { return a.time < b.time; });
    }
    partition.events.swap(grouped);
    std::sort(partition.clicks.begin(), partition.clicks.end(), [](const auto& a, const auto& b) {
        if (a.person != b.person) return a.person < b.person;
        if (a.time != b.time) return a.time < b.time;
        return b.id < a.id;
    });
    // Identical click copies have the same person and timestamp and are now adjacent.
    const auto unique_end = std::unique(partition.clicks.begin(), partition.clicks.end(),
        [](const auto& a, const auto& b) {
            return a.time == b.time && a.person == b.person && a.id == b.id;
        });
    partition.clicks.erase(unique_end, partition.clicks.end());
}

void sessions_and_users(Partition& partition, std::vector<UserDay>& users,
                        const std::vector<Text>& categories) {
    std::vector<int64_t> category_revenue(categories.size());
    std::vector<uint32_t> purchased;
    uint32_t last_person = no_index;
    Time previous = 0;
    auto flush_category = [&] {
        auto& row = users.back();
        int64_t highest = 0;
        for (uint32_t category : purchased) {
            auto& total = category_revenue[category];
            if (total > highest ||
                (total == highest && categories[category] < categories[row.category])) {
                row.category = category;
                highest = total;
            }
            total = 0;
        }
        purchased.clear();
    };

    for (const auto& event : partition.events) {
        if (event.person != last_person || event.time - previous > session_gap_ms) {
            const int32_t day = utc_day(event.time);
            if (event.person != last_person || day != users.back().day) {
                if (!users.empty()) flush_category();
                users.push_back({partition.names[event.person], day, no_category, 0, 0, 0});
            }
            partition.sessions.push_back({event.time, 0, event.person});
            ++users.back().sessions;
        }
        previous = event.time;
        last_person = event.person;
        auto& row = users.back();
        ++row.events;
        if (event.price) {
            row.revenue += event.price;
            partition.sessions.back().revenue += event.price;
            auto& total = category_revenue[event.category];
            if (!total) purchased.push_back(event.category);
            total += event.price;
        }
    }
    if (!users.empty()) flush_category();
    std::vector<IndexedEvent>().swap(partition.events);
}

class ActiveCampaigns {
public:
    explicit ActiveCampaigns(size_t count) : counts_(count), positions_(count) {}

    void add(uint32_t campaign) {
        if (counts_[campaign]++ == 0) {
            positions_[campaign] = active.size();
            active.push_back(campaign);
        }
    }

    void remove(uint32_t campaign) {
        if (--counts_[campaign] == 0) {
            const auto tail = active.back();
            const auto position = positions_[campaign];
            active[position] = tail;
            positions_[tail] = position;
            active.pop_back();
        }
    }

    void clear() {
        for (auto campaign : active) counts_[campaign] = 0;
        active.clear();
    }

    std::vector<uint32_t> active;
private:
    std::vector<size_t> counts_, positions_;
};

void attribute(Partition& partition, CampaignDays& totals, size_t campaigns) {
    const auto& clicks = partition.clicks;
    ActiveCampaigns active(campaigns);
    uint32_t last_person = no_index;
    size_t first = 0, next = 0;
    for (const auto& session : partition.sessions) {
        if (session.person != last_person) {
            active.clear();
            while (next < clicks.size() && clicks[next].person < session.person) ++next;
            first = next;
            last_person = session.person;
        }
        const Time cutoff = session.start - week_ms;
        while (first < next && clicks[first].time < cutoff) {
            active.remove(clicks[first++].campaign);
        }
        // On the first session or after a long gap, discard clicks that are already too old.
        while (next < clicks.size() && clicks[next].person == session.person &&
               clicks[next].time < cutoff) {
            ++next;
            ++first;
        }
        while (next < clicks.size() && clicks[next].person == session.person &&
               clicks[next].time <= session.start) {
            active.add(clicks[next++].campaign);
        }
        // Click IDs are descending within a timestamp, so the latest entry wins ties.
        const uint32_t credited = first == next ? 0 : clicks[next - 1].campaign;
        const int32_t day = utc_day(session.start);
        auto& row = totals[campaign_day_key(credited, day)];
        ++row.sessions;
        row.purchasing_sessions += (session.revenue > 0);
        row.revenue += session.revenue;
        for (auto campaign : active.active) ++totals[campaign_day_key(campaign, day)].assisted_sessions;
    }
    std::vector<Session>().swap(partition.sessions);
    std::vector<IndexedClick>().swap(partition.clicks);
}

const char* environment(const char* key, const char* fallback) {
    const auto* value = std::getenv(key);
    return value && *value ? value : fallback;
}

int main(int argc, char** argv) {
    try {
        if (argc != 1 && argc != 3) throw std::runtime_error("Usage: shopping-sessions [DATA_DIR OUTPUT_DIR]");
        const fs::path data = argc == 3 ? argv[1] : environment("DATA_DIR", "/data");
        const fs::path output = argc == 3 ? argv[2] : environment("OUTPUT_DIR", "/output");
        const int workers = std::clamp(std::stoi(environment("NCPU", "8")), 1, 64);
        Timings timings;
        const Window window = window_from_manifest(data / "manifest.json");
        const InputParts link_files(data / "links");
        std::optional<Identities> identities;
        std::promise<const Identities*> graph;
        const auto graph_ready = graph.get_future().share();
        const Lookups lookups(data);
        timings.mark("setup");

        const InputParts event_files(data / "events"), click_files(data / "clicks");
        std::vector<EventCopies> copies(workers);
        std::vector<Clicks> clicks(workers);
        std::vector<KeyArena> keys(workers);
        const size_t event_chunks = event_files.chunks.size();
        parallel(1 + event_chunks + click_files.chunks.size(), workers, [&](size_t worker, size_t index) {
            if (index == 0) {
                try {
                    graph.set_value(&identities.emplace(link_files));
                } catch (...) {
                    graph.set_exception(std::current_exception());
                    throw;
                }
            } else if (index <= event_chunks) {
                parse_events(event_files.chunks[index - 1], copies[worker], window, lookups, keys[worker]);
            } else {
                parse_clicks(click_files.chunks[index - event_chunks - 1], clicks[worker],
                             window, lookups, *graph_ready.get(), keys[worker]);
            }
        });
        timings.mark("input_parse_and_identity");

        std::vector<Events> events(workers);
        parallel(event_shards, workers, [&](size_t worker, size_t shard) {
            deduplicate(copies, shard, events[worker], *identities);
        });
        timings.mark("event_dedup");

        std::vector<Partition> partitions(person_shards);
        parallel(person_shards, workers, [&](size_t, size_t shard) {
            index_and_sort(shard, events, clicks, partitions[shard]);
        });
        timings.mark("person_index_and_sort");

        std::vector<std::vector<UserDay>> user_rows(person_shards);
        parallel(person_shards, workers, [&](size_t, size_t shard) {
            sessions_and_users(partitions[shard], user_rows[shard], lookups.categories);
        });
        timings.mark("sessionize_and_user_aggregate");

        std::vector<CampaignDays> campaign_rows(workers);
        parallel(person_shards, workers, [&](size_t worker, size_t shard) {
            attribute(partitions[shard], campaign_rows[worker], lookups.campaigns.size());
        });
        timings.mark("attribution_and_assists");

        auto& totals = campaign_rows.front();
        for (size_t i = 1; i < campaign_rows.size(); ++i) {
            for (const auto& [key, value] : campaign_rows[i]) totals[key].add(value);
        }
        timings.mark("campaign_aggregate");

        fs::create_directories(output / "user_daily");
        fs::create_directories(output / "campaign_daily");
        constexpr size_t output_parts = 8;
        parallel(output_parts + 1, workers, [&](size_t, size_t part) {
            if (part == output_parts) {
                write_campaigns(output / "campaign_daily" / "part-0.parquet", totals, lookups.campaigns);
                return;
            }
            const size_t start = part * person_shards / output_parts;
            const size_t end = (part + 1) * person_shards / output_parts;
            const auto filename = "part-" + std::to_string(part) + ".parquet";
            write_users(output / "user_daily" / filename, user_rows, start, end, lookups.categories);
        });
        timings.mark("parquet_output");
        timings.total();
    } catch (const std::exception& error) {
        std::cerr << "shopping-sessions: " << error.what() << '\n';
        return 1;
    }
}
