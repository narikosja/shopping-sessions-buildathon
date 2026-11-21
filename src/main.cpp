#include "identifiers.hpp"
#include "input.hpp"
#include "output.hpp"

#include <atomic>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sched.h>
#include <thread>
#include <unordered_map>

namespace shopping {

constexpr size_t event_shards = 256;
constexpr size_t person_shards = 128;

template <class T>
using StringMap = std::unordered_map<Text, T, TextHash>;

template <class Work>
void parallel(size_t jobs, size_t threads, Work work) {
    threads = std::min(jobs, threads);
    if (threads <= 1) {
        for (size_t i = 0; i < jobs; ++i) work(i, 0);
        return;
    }

    std::atomic<size_t> next = 0;
    std::exception_ptr failure;
    std::mutex failure_mutex;
    auto run = [&](size_t thread) {
        try {
            for (;;) {
                size_t job = next.fetch_add(1, std::memory_order_relaxed);
                if (job >= jobs) break;
                work(job, thread);
            }
        } catch (...) {
            std::lock_guard lock(failure_mutex);
            if (!failure) failure = std::current_exception();
            next.store(jobs, std::memory_order_relaxed);
        }
    };
    std::vector<std::jthread> workers;
    for (size_t i = 1; i < threads; ++i) workers.emplace_back(run, i);
    run(0);
    workers.clear();
    if (failure) std::rethrow_exception(failure);
}

class Links {
public:
    Links(const std::vector<Text>& chunks, Identifiers& identifiers) {
        size_t bytes = 0;
        for (const Text chunk : chunks) bytes += chunk.size();
        aliases_.resize(std::bit_ceil(bytes / 8 + 1));
        nodes_.reserve(bytes / 12);
        for (Text chunk : chunks) {
            csv_rows<2>(chunk, [&](const auto& row) {
                if (row[0].empty() || row[1].empty()) return;
                uint32_t a = root(intern(identifiers.get(row[0])));
                uint32_t b = root(intern(identifiers.get(row[1])));
                if (a == b) return;
                if (nodes_[a].size < nodes_[b].size) std::swap(a, b);
                nodes_[b].parent = a;
                nodes_[a].size += nodes_[b].size;
                if (Identifiers::byte_less(nodes_[b].name, nodes_[a].name)) {
                    nodes_[a].name = nodes_[b].name;
                }
            });
        }
        for (auto& alias : aliases_) {
            if (alias.alias) alias.person = nodes_[root(alias.person)].name;
        }
        std::vector<Node>().swap(nodes_);
    }

    Id canonical(Id alias) const {
        size_t slot = alias_slot(alias);
        while (aliases_[slot].alias) {
            if (aliases_[slot].alias == alias) return aliases_[slot].person;
            slot = next_slot(slot);
        }
        return alias;
    }

private:
    struct Node {
        uint32_t parent;
        uint32_t size;
        Id name;
    };

    struct Alias {
        Id alias = 0;
        Id person = 0;
    };

    size_t alias_slot(Id alias) const { return mix(alias) & (aliases_.size() - 1); }
    size_t next_slot(size_t slot) const { return (slot + 1) & (aliases_.size() - 1); }

    uint32_t intern(Id user) {
        size_t slot = alias_slot(user);
        while (aliases_[slot].alias && aliases_[slot].alias != user) slot = next_slot(slot);
        if (aliases_[slot].alias) return aliases_[slot].person;

        if (nodes_.size() >= aliases_.size() * 3 / 4) {
            auto previous = std::move(aliases_);
            aliases_ = std::vector<Alias>(previous.size() * 2);
            for (const auto& entry : previous) {
                if (!entry.alias) continue;
                size_t target = alias_slot(entry.alias);
                while (aliases_[target].alias) target = next_slot(target);
                aliases_[target] = entry;
            }
            slot = alias_slot(user);
            while (aliases_[slot].alias) slot = next_slot(slot);
        }
        const auto index = static_cast<uint32_t>(nodes_.size());
        aliases_[slot] = {user, index};
        nodes_.push_back({index, 1, user});
        return index;
    }

    uint32_t root(uint32_t id) {
        while (nodes_[id].parent != id) {
            auto& parent = nodes_[id].parent;
            parent = nodes_[parent].parent;
            id = parent;
        }
        return id;
    }

    std::vector<Node> nodes_;
    std::vector<Alias> aliases_;
};

struct Catalogs {
    StringMap<uint32_t> products;
    StringMap<uint32_t> campaigns;
    std::vector<Text> categories{"unknown"};
    std::vector<Campaign> campaign_rows{{"organic", "organic"}};

    Catalogs(Text products_csv, Text campaigns_csv) {
        StringMap<uint32_t> category_ids;
        csv_rows<2>(products_csv, [&](const auto& row) {
            auto [it, inserted] = category_ids.try_emplace(row[1], categories.size());
            if (inserted) categories.push_back(row[1]);
            products.emplace(row[0], it->second);
        });
        csv_rows<2>(campaigns_csv, [&](const auto& row) {
            campaigns.emplace(row[0], campaign_rows.size());
            campaign_rows.push_back({row[0], row[1]});
        });
    }
};

struct CompactEventData {
    uint32_t time = 0;
    uint32_t purchase = 0;
    static constexpr uint32_t price_mask = (1u << 24) - 1;

    CompactEventData() = default;
    CompactEventData(int64_t ts, uint32_t price, uint32_t category, int64_t origin)
        : time(static_cast<uint32_t>(ts - origin)), purchase(price | (category << 24)) {}

    int64_t timestamp(int64_t origin) const { return origin + time; }
    uint32_t price() const { return purchase & price_mask; }
    uint32_t category() const { return purchase >> 24; }
};

struct WideEventData {
    int64_t time = 0;
    uint32_t cents = 0;
    uint32_t category_id = 0;

    WideEventData() = default;
    WideEventData(int64_t ts, uint32_t price, uint32_t category, int64_t)
        : time(ts), cents(price), category_id(category) {}

    int64_t timestamp(int64_t) const { return time; }
    uint32_t price() const { return cents; }
    uint32_t category() const { return category_id; }
};

template <class Data>
struct Event {
    Id user = 0;
    Data data;
};

template <class Data>
struct EventCandidate {
    Id id;
    int64_t ingest;
    Event<Data> event;
};

struct Click {
    Id user;
    int64_t ts;
    Id id;
    uint32_t campaign;
};

template <class Data>
EventCandidate<Data> parse_event(const std::array<Text, 7>& row, const Window& window,
                           const Catalogs& catalogs, Identifiers& identifiers) {
    EventCandidate<Data> event{identifiers.get(row[0]), iso_timestamp(row[3]), {}};
    uint32_t cents = 0;
    uint32_t category = 0;
    if (row[1].empty() || row[5].empty()) return event;
    const int64_t ts = timestamp(row[2]);
    if (ts < window.start || ts >= window.end || event.ingest - ts > day_ms) {
        return event;
    }
    if (row[4] == "purchase") {
        const int64_t price = unsigned_value(row[6], 10'000'000);
        if (price < 1) return event;
        cents = static_cast<uint32_t>(price);
        const auto product = catalogs.products.find(row[5]);
        if (product != catalogs.products.end()) category = product->second;
    } else if (row[4] != "view" && row[4] != "cart") {
        return event;
    }
    event.event = {identifiers.get(row[1]), Data(ts, cents, category, window.start)};
    return event;
}

template <class Data>
using EventInput = std::array<std::vector<EventCandidate<Data>>, event_shards>;
template <class Data>
using EventPartitions = std::array<std::vector<Event<Data>>, person_shards>;
using ClickPartitions = std::array<std::vector<Click>, person_shards>;

template <class Data>
void deduplicate(size_t shard, std::vector<EventInput<Data>>& inputs, EventPartitions<Data>& output,
                 const Links& links) {
    size_t count = 0;
    for (const auto& input : inputs) count += input[shard].size();
    constexpr size_t slot_shift = std::countr_zero(event_shards);
    if (count == 0) return;
    struct Slot {
        Id id = 0;
        EventCandidate<Data>* row = nullptr;
    };
    std::vector<Slot> slots(std::bit_ceil(count + count / 2 + 1));
    const size_t mask = slots.size() - 1;
    for (auto& input : inputs) {
        for (auto& row : input[shard]) {
            size_t i = (mix(row.id) >> slot_shift) & mask;
            while (slots[i].id && slots[i].id != row.id) i = (i + 1) & mask;
            auto& slot = slots[i];
            if (!slot.id) {
                slot = {row.id, &row};
            } else if (row.ingest < slot.row->ingest) {
                slot.row->event.user = 0;
                slot.row = &row;
            } else {
                row.event.user = 0;
            }
        }
    }
    for (auto& input : inputs) {
        for (const auto& row : input[shard]) {
            if (!row.event.user) continue;
            const Id person = links.canonical(row.event.user);
            output[mix(person) & (person_shards - 1)].push_back({person, row.event.data});
        }
        std::vector<EventCandidate<Data>>().swap(input[shard]);
    }
}

using DailyCampaigns = std::unordered_map<int32_t, std::vector<CampaignDay>>;

class ActiveCampaigns {
public:
    explicit ActiveCampaigns(size_t size) : entries_(size) {}

    void add(uint32_t campaign) {
        auto& entry = entries_[campaign];
        if (entry.count++ == 0) {
            entry.index = active.size();
            active.push_back(campaign);
        }
    }

    void remove(uint32_t campaign) {
        auto& entry = entries_[campaign];
        if (--entry.count == 0) {
            active[entry.index] = active.back();
            entries_[active.back()].index = entry.index;
            active.pop_back();
        }
    }

    std::vector<uint32_t> active;

private:
    struct Entry {
        size_t count = 0;
        size_t index = 0;
    };
    std::vector<Entry> entries_;
};

template <class Data>
void aggregate(const std::vector<Event<Data>>& events, const std::vector<Click>& clicks,
               const Catalogs& catalogs, std::vector<UserDay>& users, DailyCampaigns& campaign_days,
               int64_t origin) {
    const size_t campaign_count = catalogs.campaign_rows.size();
    ActiveCampaigns campaigns(campaign_count);
    std::vector<int64_t> category_revenue(catalogs.categories.size());
    std::vector<uint32_t> purchased;
    size_t click = 0;
    size_t event = 0;
    while (event < events.size()) {
        const Id user = events[event].user;
        while (click < clicks.size() && clicks[click].user < user) ++click;
        size_t oldest = click;

        do {
            const int32_t day = utc_day(events[event].data.timestamp(origin));
            UserDay row{user, day, 0, 0, 0, {}};
            auto& daily = campaign_days.try_emplace(day, campaign_count).first->second;
            do {
                const auto start = events[event].data.timestamp(origin);
                const size_t first = event;
                int64_t revenue = 0;
                int64_t last = start;
                do {
                    const auto& item = events[event++];
                    last = item.data.timestamp(origin);
                    const auto price = item.data.price();
                    if (price != 0) {
                        const auto category = item.data.category();
                        revenue += price;
                        if (category_revenue[category] == 0) purchased.push_back(category);
                        category_revenue[category] += price;
                    }
                } while (event < events.size() && events[event].user == user &&
                         events[event].data.timestamp(origin) - last <= gap_ms);

                row.sessions++;
                row.events += event - first;
                row.revenue += revenue;
                while (click < clicks.size() && clicks[click].ts <= start && clicks[click].user == user) {
                    campaigns.add(clicks[click++].campaign);
                }
                while (oldest < click && clicks[oldest].ts < start - lookback_ms) {
                    campaigns.remove(clicks[oldest++].campaign);
                }
                // Equal timestamps are ordered by descending click ID, so the smallest is last.
                const uint32_t credited = oldest < click ? clicks[click - 1].campaign : 0;
                auto& credit = daily[credited];
                credit.sessions++;
                credit.purchasing += revenue != 0;
                credit.revenue += revenue;
                for (uint32_t campaign : campaigns.active) daily[campaign].assists++;
            } while (event < events.size() && events[event].user == user &&
                     utc_day(events[event].data.timestamp(origin)) == day);

            int64_t highest = 0;
            for (uint32_t category : purchased) {
                const Text name = catalogs.categories[category];
                const auto revenue = category_revenue[category];
                if (revenue > highest || (revenue == highest && name < row.category)) {
                    highest = revenue;
                    row.category = name;
                }
                category_revenue[category] = 0;
            }
            purchased.clear();
            users.push_back(row);
        } while (event < events.size() && events[event].user == user);

        while (oldest < click) campaigns.remove(clicks[oldest++].campaign);
    }
}

void write_campaigns(const fs::path& path, const Catalogs& catalogs,
                     const std::array<DailyCampaigns, person_shards>& parts) {
    DailyCampaigns merged;
    for (const auto& part : parts) {
        for (const auto& [day, rows] : part) {
            auto& totals = merged.try_emplace(day, rows.size()).first->second;
            for (size_t i = 0; i < rows.size(); ++i) totals[i] += rows[i];
        }
    }
    arrow::StringBuilder id, channel;
    arrow::Date32Builder day_builder;
    arrow::Int64Builder sessions, purchasing, revenue, assists;
    for (const auto& [day, values] : merged) {
        for (size_t i = 0; i < values.size(); ++i) {
            const auto& row = values[i];
            if (row.sessions == 0 && row.assists == 0) continue;
            check(id.Append(catalogs.campaign_rows[i].id));
            check(channel.Append(catalogs.campaign_rows[i].channel));
            check(day_builder.Append(day));
            check(sessions.Append(row.sessions));
            check(purchasing.Append(row.purchasing));
            check(revenue.Append(row.revenue));
            check(assists.Append(row.assists));
        }
    }
    write_table(path, campaign_schema(), id.length(),
                {&id, &channel, &day_builder, &sessions, &purchasing, &revenue, &assists});
}

const char* environment(const char* key, const char* fallback) {
    const char* value = std::getenv(key);
    return value && *value ? value : fallback;
}

size_t worker_count() {
    cpu_set_t cpus;
    size_t available = sched_getaffinity(0, sizeof(cpus), &cpus) == 0
        ? CPU_COUNT(&cpus) : std::thread::hardware_concurrency();
    available = std::max<size_t>(1, available);
    const Text configured = environment("NCPU", "");
    int64_t requested = unsigned_value(configured, std::numeric_limits<int>::max());
    if (requested > 0) available = std::min<size_t>(available, requested);
    return std::min<size_t>(available, 32);
}

class Timings {
public:
    void stage(const char* name) {
        const auto now = Clock::now();
        if (std::getenv("PROFILE")) {
            std::cerr << name << ' ' << std::chrono::duration<double>(now - last_).count() << " s\n";
        }
        last_ = now;
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point last_ = Clock::now();
};

template <class Data>
void process(const Window& window, const Catalogs& catalogs, const Links& links,
             Identifiers& identifiers, const std::vector<Text>& event_chunks,
             const std::vector<Text>& click_chunks, const fs::path& output, size_t threads,
             Timings& timing) {
    std::vector<EventInput<Data>> parsed(threads);
    size_t event_bytes = 0;
    for (const auto& chunk : event_chunks) event_bytes += chunk.size();
    const size_t initial_capacity = event_bytes / 68 / event_shards / threads;
    for (auto& worker : parsed) {
        for (auto& shard : worker) shard.reserve(initial_capacity);
    }
    parallel(event_chunks.size(), threads, [&](size_t chunk, size_t worker) {
        csv_rows<7>(event_chunks[chunk], [&](const auto& row) {
            if (row[0].empty()) return;
            auto event = parse_event<Data>(row, window, catalogs, identifiers);
            parsed[worker][mix(event.id) & (event_shards - 1)].push_back(event);
        });
    });
    timing.stage("Parse events");

    std::vector<EventPartitions<Data>> partitioned(threads);
    parallel(event_shards, threads, [&](size_t shard, size_t worker) {
        deduplicate(shard, parsed, partitioned[worker], links);
    });
    parsed.clear();
    timing.stage("Deduplicate events and resolve user IDs");

    std::vector<ClickPartitions> click_parts(threads);
    parallel(click_chunks.size(), threads, [&](size_t chunk, size_t worker) {
        csv_rows<4>(click_chunks[chunk], [&](const auto& row) {
            if (row[0].empty() || row[1].empty()) return;
            const int64_t ts = timestamp(row[3]);
            if (ts < window.start - lookback_ms || ts >= window.end) return;
            const auto campaign = catalogs.campaigns.find(row[2]);
            if (campaign == catalogs.campaigns.end()) return;
            const Id user = links.canonical(identifiers.get(row[1]));
            // Identical duplicates have no effect on the latest click or active campaign set.
            click_parts[worker][mix(user) & (person_shards - 1)].push_back(
                {user, ts, identifiers.get(row[0]), campaign->second});
        });
    });
    timing.stage("Parse clicks and resolve user IDs");

    EventPartitions<Data> events;
    ClickPartitions clicks;
    parallel(person_shards, threads, [&](size_t shard, size_t) {
        auto gather = [shard](auto& parts, auto& rows) {
            size_t count = 0;
            for (const auto& part : parts) count += part[shard].size();
            rows.reserve(count);
            for (auto& part : parts) {
                auto& source = part[shard];
                rows.insert(rows.end(), source.begin(), source.end());
                std::remove_reference_t<decltype(source)>().swap(source);
            }
        };
        gather(partitioned, events[shard]);
        gather(click_parts, clicks[shard]);
        std::sort(events[shard].begin(), events[shard].end(), [](const Event<Data>& a, const Event<Data>& b) {
            if (a.user == b.user) return a.data.time < b.data.time;
            return a.user < b.user;
        });
        std::sort(clicks[shard].begin(), clicks[shard].end(), [](const Click& a, const Click& b) {
            if (a.user == b.user) {
                if (a.ts == b.ts) return Identifiers::byte_less(b.id, a.id);
                return a.ts < b.ts;
            }
            return a.user < b.user;
        });
    });
    timing.stage("Sort events and clicks");

    std::array<std::vector<UserDay>, person_shards> user_days;
    std::array<DailyCampaigns, person_shards> campaign_days;
    parallel(person_shards, threads, [&](size_t shard, size_t) {
        aggregate(events[shard], clicks[shard], catalogs, user_days[shard], campaign_days[shard], window.start);
        std::vector<Event<Data>>().swap(events[shard]);
        std::vector<Click>().swap(clicks[shard]);
    });
    timing.stage("Sessionize, attribute and aggregate");

    const fs::path users_dir = output / "user_daily";
    const fs::path campaigns_dir = output / "campaign_daily";
    fs::create_directories(users_dir);
    fs::create_directories(campaigns_dir);
    constexpr size_t writers = 8;
    parallel(writers + 1, threads, [&](size_t part, size_t) {
        if (part == writers) {
            write_campaigns(campaigns_dir / "part-0.parquet", catalogs, campaign_days);
            return;
        }
        std::vector<UserDay> rows;
        size_t count = 0;
        for (size_t shard = part; shard < person_shards; shard += writers) {
            count += user_days[shard].size();
        }
        rows.reserve(count);
        for (size_t shard = part; shard < person_shards; shard += writers) {
            rows.insert(rows.end(), user_days[shard].begin(), user_days[shard].end());
        }
        write_users(users_dir / ("part-" + std::to_string(part) + ".parquet"), rows);
    });
    timing.stage("Write Parquet outputs");
}

void run(const fs::path& data, const fs::path& output, size_t threads) {
    Timings timing;
    Inputs inputs;
    const Window window = read_window(data / "manifest.json");
    const Catalogs catalogs(inputs.load(data / "products.csv"), inputs.load(data / "campaigns.csv"));
    Identifiers identifiers;
    const Links links(inputs.chunks(data / "links"), identifiers);
    const auto event_chunks = inputs.chunks(data / "events");
    const auto click_chunks = inputs.chunks(data / "clicks");
    timing.stage("Catalogs and identity links");

    // With the official 14-day window, relative millisecond timestamps and
    // the catalog's category indices fit in 8 bytes. Wide datasets use the same pipeline.
    if (window.end - window.start <= std::numeric_limits<uint32_t>::max() &&
        catalogs.categories.size() <= 256) {
        process<CompactEventData>(window, catalogs, links, identifiers, event_chunks,
                                  click_chunks, output, threads, timing);
    } else {
        process<WideEventData>(window, catalogs, links, identifiers, event_chunks,
                               click_chunks, output, threads, timing);
    }
}

}  // namespace shopping

int main(int argc, char** argv) {
    try {
        if (argc != 1 && argc != 3) throw std::runtime_error("Usage: shopping-sessions [DATA OUTPUT]");
        const char* data = argc == 3 ? argv[1] : shopping::environment("DATA_DIR", "/data");
        const char* output = argc == 3 ? argv[2] : shopping::environment("OUTPUT_DIR", "/output");
        shopping::run(data, output, shopping::worker_count());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "shopping-sessions: " << error.what() << '\n';
        return 1;
    }
}
