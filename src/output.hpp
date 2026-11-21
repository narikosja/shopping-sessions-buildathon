#pragma once

#include "input.hpp"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

namespace shopping {

struct UserDay {
    Text user;
    int32_t day;
    int64_t sessions;
    int64_t events;
    int64_t revenue;
    Text category;
};

struct CampaignDay {
    int64_t sessions = 0;
    int64_t purchasing = 0;
    int64_t revenue = 0;
    int64_t assists = 0;

    CampaignDay& operator+=(const CampaignDay& other) {
        sessions += other.sessions;
        purchasing += other.purchasing;
        revenue += other.revenue;
        assists += other.assists;
        return *this;
    }
};

struct Campaign {
    Text id;
    Text channel;
};

inline void check(arrow::Status status) {
    if (!status.ok()) throw std::runtime_error(status.ToString());
}

template <class T>
T unwrap(arrow::Result<T> result) {
    check(result.status());
    return std::move(result).ValueUnsafe();
}

inline std::shared_ptr<arrow::Schema> user_schema() {
    return arrow::schema({
        arrow::field("user_id", arrow::utf8(), false),
        arrow::field("day", arrow::date32(), false),
        arrow::field("sessions", arrow::int64(), false),
        arrow::field("events", arrow::int64(), false),
        arrow::field("revenue_cents", arrow::int64(), false),
        arrow::field("top_category", arrow::utf8())});
}

inline std::shared_ptr<arrow::Schema> campaign_schema() {
    return arrow::schema({
        arrow::field("campaign_id", arrow::utf8(), false),
        arrow::field("channel", arrow::utf8(), false),
        arrow::field("day", arrow::date32(), false),
        arrow::field("sessions", arrow::int64(), false),
        arrow::field("purchasing_sessions", arrow::int64(), false),
        arrow::field("revenue_cents", arrow::int64(), false),
        arrow::field("assisted_sessions", arrow::int64(), false)});
}

inline void write_table(const fs::path& path, const std::shared_ptr<arrow::Schema>& schema,
                        int64_t rows, std::initializer_list<arrow::ArrayBuilder*> builders) {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (auto* builder : builders) columns.push_back(unwrap(builder->Finish()));
    auto batch = arrow::RecordBatch::Make(schema, rows, std::move(columns));
    auto table = unwrap(arrow::Table::FromRecordBatches(schema, {batch}));
    auto file = unwrap(arrow::io::FileOutputStream::Open(path.string()));
    auto properties = parquet::WriterProperties::Builder()
                          .compression(parquet::Compression::SNAPPY)
                          ->build();
    check(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), file, 65'536, properties));
    check(file->Close());
}

inline void write_users(const fs::path& path, const std::vector<UserDay>& rows) {
    arrow::StringBuilder user, category;
    arrow::Date32Builder day;
    arrow::Int64Builder sessions, events, revenue;
    for (arrow::ArrayBuilder* builder : std::array<arrow::ArrayBuilder*, 6>{
             &user, &day, &sessions, &events, &revenue, &category}) {
        check(builder->Reserve(rows.size()));
    }
    size_t id_bytes = 0;
    size_t category_bytes = 0;
    for (const auto& row : rows) {
        id_bytes += row.user.size();
        category_bytes += row.category.size();
    }
    check(user.ReserveData(id_bytes));
    check(category.ReserveData(category_bytes));
    for (const auto& row : rows) {
        user.UnsafeAppend(row.user);
        day.UnsafeAppend(row.day);
        sessions.UnsafeAppend(row.sessions);
        events.UnsafeAppend(row.events);
        revenue.UnsafeAppend(row.revenue);
        if (row.category.data() == nullptr) category.UnsafeAppendNull();
        else category.UnsafeAppend(row.category);
    }
    write_table(path, user_schema(), rows.size(), {&user, &day, &sessions, &events, &revenue, &category});
}

}  // namespace shopping
