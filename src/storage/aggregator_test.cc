/*
 * Copyright 2021 4Paradigm
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <map>
#include <utility>
#include "gtest/gtest.h"

#include "codec/schema_codec.h"
#include "common/timer.h"
#include "storage/aggregator.h"
#include "storage/mem_table.h"
namespace openmldb {
namespace storage {

using ::openmldb::codec::SchemaCodec;

uint32_t counter = 10;

class AggregatorTest : public ::testing::Test {
 public:
    AggregatorTest() {}
    ~AggregatorTest() {}
};

void AddDefaultAggregatorBaseSchema(::openmldb::api::TableMeta* table_meta) {
    table_meta->set_name("t0");
    table_meta->set_pid(0);
    table_meta->set_mode(::openmldb::api::TableMode::kTableLeader);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "id1", openmldb::type::DataType::kString);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "id2", openmldb::type::DataType::kString);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "ts_col", openmldb::type::DataType::kTimestamp);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "col3", openmldb::type::DataType::kInt);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "col4", openmldb::type::DataType::kSmallInt);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "col5", openmldb::type::DataType::kBigInt);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "col6", openmldb::type::DataType::kFloat);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "col7", openmldb::type::DataType::kDouble);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "col8", openmldb::type::DataType::kDate);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "col9", openmldb::type::DataType::kString);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "col_null", openmldb::type::DataType::kInt);

    SchemaCodec::SetIndex(table_meta->add_column_key(), "idx", "id1|id2", "ts_col", ::openmldb::type::kAbsoluteTime, 0,
                          0);
    return;
}

void AddDefaultAggregatorSchema(::openmldb::api::TableMeta* table_meta) {
    table_meta->set_name("pre_aggr_1");
    table_meta->set_pid(0);
    table_meta->set_mode(::openmldb::api::TableMode::kTableLeader);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "key", openmldb::type::DataType::kString);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "ts_start", openmldb::type::DataType::kTimestamp);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "ts_end", openmldb::type::DataType::kTimestamp);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "num_rows", openmldb::type::DataType::kInt);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "agg_val", openmldb::type::DataType::kString);
    SchemaCodec::SetColumnDesc(table_meta->add_column_desc(), "binlog_offset", openmldb::type::DataType::kBigInt);

    SchemaCodec::SetIndex(table_meta->add_column_key(), "key", "key", "ts_start", ::openmldb::type::kAbsoluteTime, 0,
                          0);
}

bool UpdateAggr(std::shared_ptr<Aggregator> aggr, codec::RowBuilder* row_builder) {
    std::string encoded_row;
    uint32_t row_size = row_builder->CalTotalLength(9);
    encoded_row.resize(row_size);
    auto window_size = aggr->GetWindowSize();
    for (int i = 0; i <= 100; i++) {
        row_builder->SetBuffer(reinterpret_cast<int8_t*>(&(encoded_row[0])), row_size);
        row_builder->AppendString("id1", 3);
        row_builder->AppendString("id2", 3);
        row_builder->AppendTimestamp(static_cast<int64_t>(i) * window_size / 2);
        row_builder->AppendInt32(i);
        row_builder->AppendInt16(i);
        row_builder->AppendInt64(i);
        row_builder->AppendFloat(static_cast<float>(i));
        row_builder->AppendDouble(static_cast<double>(i));
        row_builder->AppendDate(i);
        row_builder->AppendString("abc", 3);
        row_builder->AppendNULL();
        bool ok = aggr->Update("id1|id2", encoded_row, i);
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool GetUpdatedResult(const uint32_t& id, const std::string& aggr_col, const std::string& aggr_type,
                           const std::string& bucket_size, AggrBuffer* last_buffer, std::shared_ptr<Table>& table) {
    ::openmldb::api::TableMeta base_table_meta;
    base_table_meta.set_tid(id);
    AddDefaultAggregatorBaseSchema(&base_table_meta);
    ::openmldb::api::TableMeta aggr_table_meta;
    aggr_table_meta.set_tid(id + 1);
    AddDefaultAggregatorSchema(&aggr_table_meta);
    std::shared_ptr<Table> aggr_table = std::make_shared<MemTable>(aggr_table_meta);
    aggr_table->Init();
    auto aggr =
        CreateAggregator(base_table_meta, aggr_table_meta, aggr_table, 0, aggr_col, aggr_type, "ts_col", bucket_size);
    codec::RowBuilder row_builder(base_table_meta.column_desc());
    UpdateAggr(aggr, &row_builder);
    std::string key = "id1|id2";
    auto ok = aggr->GetAggrBuffer(key, last_buffer);
    if (!ok) {
        return false;
    }
    table = aggr_table;
    return true;
}

template<typename T>
bool CheckSumAggrResult(std::shared_ptr<Table> aggr_table, DataType data_type) {
    if (!(aggr_table->GetRecordCnt() == 50)) {
        return false;
    }
    auto it = aggr_table->NewTraverseIterator(0);
    it->SeekToFirst();
    for (int i = 50 - 1; i >= 0; --i) {
        if (!it->Valid()) {
            return false;
        }
        auto tmp_val = it->GetValue();
        std::string origin_data = tmp_val.ToString();
        codec::RowView origin_row_view(aggr_table->GetTableMeta()->column_desc(),
                                       reinterpret_cast<int8_t*>(const_cast<char*>(origin_data.c_str())),
                                       origin_data.size());
        char* ch = NULL;
        uint32_t ch_length = 0;
        origin_row_view.GetString(4, &ch, &ch_length);
        T origin_val = *(reinterpret_cast<T*>(ch));
        if (origin_val != static_cast<T>(i * 4 + 1)) {
            return false;
        }
        it->Next();
    }
    return true;
}

template<typename T>
bool CheckMinAggrResult(std::shared_ptr<Table> aggr_table, DataType data_type) {
    if (!(aggr_table->GetRecordCnt() == 50)) {
        return false;
    }
    auto it = aggr_table->NewTraverseIterator(0);
    it->SeekToFirst();
    for (int i = 50 - 1; i >= 0; --i) {
        if (!it->Valid()) {
            return false;
        }
        auto tmp_val = it->GetValue();
        std::string origin_data = tmp_val.ToString();
        codec::RowView origin_row_view(aggr_table->GetTableMeta()->column_desc(),
                                       reinterpret_cast<int8_t*>(const_cast<char*>(origin_data.c_str())),
                                       origin_data.size());
        char* ch = NULL;
        uint32_t ch_length = 0;
        origin_row_view.GetString(4, &ch, &ch_length);
        T origin_val = *reinterpret_cast<T*>(ch);
        if (origin_val != static_cast<T>(i * 2)) {
            return false;
        }
        it->Next();
    }
    return true;
}

template<typename T>
bool CheckMaxAggrResult(std::shared_ptr<Table> aggr_table, DataType data_type) {
    if (!(aggr_table->GetRecordCnt() == 50)) {
        return false;
    }
    auto it = aggr_table->NewTraverseIterator(0);
    it->SeekToFirst();
    for (int i = 50 - 1; i >= 0; --i) {
        if (!it->Valid()) {
            return false;
        }
        auto tmp_val = it->GetValue();
        std::string origin_data = tmp_val.ToString();
        codec::RowView origin_row_view(aggr_table->GetTableMeta()->column_desc(),
                                       reinterpret_cast<int8_t*>(const_cast<char*>(origin_data.c_str())),
                                       origin_data.size());
        char* ch = NULL;
        uint32_t ch_length = 0;
        origin_row_view.GetString(4, &ch, &ch_length);
        T origin_val = *reinterpret_cast<T*>(ch);
        if (origin_val != static_cast<T>(i * 2 + 1)) {
            return false;
        }
        it->Next();
    }
    return true;
}

bool CheckCountAggrResult(std::shared_ptr<Table> aggr_table, DataType data_type, int64_t count) {
    if (!(aggr_table->GetRecordCnt() == 50)) {
        return false;
    }
    auto it = aggr_table->NewTraverseIterator(0);
    it->SeekToFirst();
    for (int i = 50 - 1; i >= 0; --i) {
        if (!it->Valid()) {
            return false;
        }
        auto tmp_val = it->GetValue();
        std::string origin_data = tmp_val.ToString();
        codec::RowView origin_row_view(aggr_table->GetTableMeta()->column_desc(),
                                       reinterpret_cast<int8_t*>(const_cast<char*>(origin_data.c_str())),
                                       origin_data.size());
        char* ch = NULL;
        uint32_t ch_length = 0;
        origin_row_view.GetString(4, &ch, &ch_length);
        int64_t origin_val = *reinterpret_cast<int64_t*>(ch);
        if (origin_val != count) {
            return false;
        }
        it->Next();
    }
    return true;
}

template<typename T>
bool CheckAvgAggrResult(std::shared_ptr<Table> aggr_table, DataType data_type) {
    if (!(aggr_table->GetRecordCnt() == 50)) {
        return false;
    }
    auto it = aggr_table->NewTraverseIterator(0);
    it->SeekToFirst();
    for (int i = 50 - 1; i >= 0; --i) {
        if (!it->Valid()) {
            return false;
        }
        auto tmp_val = it->GetValue();
        std::string origin_data = tmp_val.ToString();
        codec::RowView origin_row_view(aggr_table->GetTableMeta()->column_desc(),
                                       reinterpret_cast<int8_t*>(const_cast<char*>(origin_data.c_str())),
                                       origin_data.size());
        char* ch = NULL;
        uint32_t ch_length = 0;
        origin_row_view.GetString(4, &ch, &ch_length);
        T origin_val = *(reinterpret_cast<T*>(ch));
        if (origin_val != static_cast<T>(i * 4 + 1)) {
            return false;
        }
        int64_t cnt = *(reinterpret_cast<int64_t*>(ch + sizeof(T)));
        if (cnt != 2) {
            return false;
        }
        it->Next();
    }
    return true;
}

TEST_F(AggregatorTest, CreateAggregator) {
    // rows_num window type
    {
        uint32_t id = counter++;
        ::openmldb::api::TableMeta base_table_meta;
        base_table_meta.set_tid(id);
        AddDefaultAggregatorBaseSchema(&base_table_meta);
        id = counter++;
        ::openmldb::api::TableMeta aggr_table_meta;
        aggr_table_meta.set_tid(id);
        AddDefaultAggregatorSchema(&aggr_table_meta);
        std::shared_ptr<Table> aggr_table = std::make_shared<MemTable>(aggr_table_meta);
        aggr_table->Init();
        auto aggr = CreateAggregator(base_table_meta, aggr_table_meta, aggr_table, 0, "col3", "sum", "ts_col", "1000");
        ASSERT_TRUE(aggr != nullptr);
        ASSERT_EQ(aggr->GetAggrType(), AggrType::kSum);
        ASSERT_EQ(aggr->GetWindowType(), WindowType::kRowsNum);
        ASSERT_EQ(aggr->GetWindowSize(), 1000);
    }
    // rows_range window type
    {
        uint32_t id = counter++;
        ::openmldb::api::TableMeta base_table_meta;
        base_table_meta.set_tid(id);
        AddDefaultAggregatorBaseSchema(&base_table_meta);
        id = counter++;
        ::openmldb::api::TableMeta aggr_table_meta;
        aggr_table_meta.set_tid(id);
        AddDefaultAggregatorSchema(&aggr_table_meta);
        std::shared_ptr<Table> aggr_table = std::make_shared<MemTable>(aggr_table_meta);
        aggr_table->Init();
        auto aggr = CreateAggregator(base_table_meta, aggr_table_meta, aggr_table, 0, "col3", "sum", "ts_col", "1d");
        ASSERT_TRUE(aggr != nullptr);
        ASSERT_EQ(aggr->GetAggrType(), AggrType::kSum);
        ASSERT_EQ(aggr->GetWindowType(), WindowType::kRowsRange);
        ASSERT_EQ(aggr->GetWindowSize(), 86400000);
    }
    {
        uint32_t id = counter++;
        ::openmldb::api::TableMeta base_table_meta;
        base_table_meta.set_tid(id);
        AddDefaultAggregatorBaseSchema(&base_table_meta);
        id = counter++;
        ::openmldb::api::TableMeta aggr_table_meta;
        aggr_table_meta.set_tid(id);
        AddDefaultAggregatorSchema(&aggr_table_meta);
        std::shared_ptr<Table> aggr_table = std::make_shared<MemTable>(aggr_table_meta);
        aggr_table->Init();
        auto aggr = CreateAggregator(base_table_meta, aggr_table_meta, aggr_table, 0, "col3", "sum", "ts_col", "2s");
        ASSERT_TRUE(aggr != nullptr);
        ASSERT_EQ(aggr->GetAggrType(), AggrType::kSum);
        ASSERT_EQ(aggr->GetWindowType(), WindowType::kRowsRange);
        ASSERT_EQ(aggr->GetWindowSize(), 2000);
    }
    {
        uint32_t id = counter++;
        ::openmldb::api::TableMeta base_table_meta;
        base_table_meta.set_tid(id);
        AddDefaultAggregatorBaseSchema(&base_table_meta);
        id = counter++;
        ::openmldb::api::TableMeta aggr_table_meta;
        aggr_table_meta.set_tid(id);
        AddDefaultAggregatorSchema(&aggr_table_meta);
        std::shared_ptr<Table> aggr_table = std::make_shared<MemTable>(aggr_table_meta);
        aggr_table->Init();
        auto aggr = CreateAggregator(base_table_meta, aggr_table_meta, aggr_table, 0, "col3", "sum", "ts_col", "3m");
        ASSERT_TRUE(aggr != nullptr);
        ASSERT_EQ(aggr->GetAggrType(), AggrType::kSum);
        ASSERT_EQ(aggr->GetWindowType(), WindowType::kRowsRange);
        ASSERT_EQ(aggr->GetWindowSize(), 3 * 60 * 1000);
    }
    {
        uint32_t id = counter++;
        ::openmldb::api::TableMeta base_table_meta;
        base_table_meta.set_tid(id);
        AddDefaultAggregatorBaseSchema(&base_table_meta);
        id = counter++;
        ::openmldb::api::TableMeta aggr_table_meta;
        aggr_table_meta.set_tid(id);
        AddDefaultAggregatorSchema(&aggr_table_meta);
        std::shared_ptr<Table> aggr_table = std::make_shared<MemTable>(aggr_table_meta);
        aggr_table->Init();
        auto aggr = CreateAggregator(base_table_meta, aggr_table_meta, aggr_table, 0, "col3", "sum", "ts_col", "100h");
        ASSERT_TRUE(aggr != nullptr);
        ASSERT_EQ(aggr->GetAggrType(), AggrType::kSum);
        ASSERT_EQ(aggr->GetWindowType(), WindowType::kRowsRange);
        ASSERT_EQ(aggr->GetWindowSize(), 100 * 60 * 60 * 1000);
    }
}

TEST_F(AggregatorTest, SumAggregatorUpdate) {
    // rows_num window type
    {
        uint32_t id = counter++;
        ::openmldb::api::TableMeta base_table_meta;
        base_table_meta.set_tid(id);
        AddDefaultAggregatorBaseSchema(&base_table_meta);
        id = counter++;
        ::openmldb::api::TableMeta aggr_table_meta;
        aggr_table_meta.set_tid(id);
        AddDefaultAggregatorSchema(&aggr_table_meta);
        std::map<std::string, uint32_t> mapping;
        mapping.insert(std::make_pair("idx", 0));
        std::shared_ptr<Table> aggr_table =
            std::make_shared<MemTable>("t", id, 0, 8, mapping, 0, ::openmldb::type::kAbsoluteTime);
        aggr_table->Init();
        auto aggr = CreateAggregator(base_table_meta, aggr_table_meta, aggr_table, 0, "col3", "sum", "ts_col", "2");
        codec::RowBuilder row_builder(base_table_meta.column_desc());
        ASSERT_TRUE(UpdateAggr(aggr, &row_builder));
        std::string key = "id1|id2";
        ASSERT_EQ(aggr_table->GetRecordCnt(), 50);
        auto it = aggr_table->NewTraverseIterator(0);
        it->SeekToFirst();
        for (int i = 50 - 1; i >= 0; --i) {
            ASSERT_TRUE(it->Valid());
            auto tmp_val = it->GetValue();
            std::string origin_data = tmp_val.ToString();
            codec::RowView origin_row_view(aggr_table_meta.column_desc(),
                                           reinterpret_cast<int8_t*>(const_cast<char*>(origin_data.c_str())),
                                           origin_data.size());
            char* ch = NULL;
            uint32_t ch_length = 0;
            origin_row_view.GetString(4, &ch, &ch_length);
            int32_t val = *reinterpret_cast<int32_t*>(ch);
            ASSERT_EQ(val, i * 4 + 1);
            it->Next();
        }
        AggrBuffer last_buffer;
        auto ok = aggr->GetAggrBuffer(key, &last_buffer);
        ASSERT_TRUE(ok);
        ASSERT_EQ(last_buffer.aggr_cnt_, 1);
        ASSERT_EQ(last_buffer.aggr_val_.vlong, 100);
        ASSERT_EQ(last_buffer.binlog_offset_, 100);
    }
    // rows_range window type
    {
        AggrBuffer last_buffer;
        std::shared_ptr<Table> aggr_table;
        ASSERT_TRUE(GetUpdatedResult(counter, "col3", "sum", "1s", &last_buffer, aggr_table));
        ASSERT_TRUE(CheckSumAggrResult<int64_t>(aggr_table, DataType::kInt));
        ASSERT_EQ(last_buffer.aggr_cnt_, 1);
        ASSERT_EQ(last_buffer.aggr_val_.vlong, 100);
        ASSERT_EQ(last_buffer.binlog_offset_, 100);
        counter += 2;
        ASSERT_TRUE(GetUpdatedResult(counter, "col4", "sum", "1m", &last_buffer, aggr_table));
        ASSERT_TRUE(CheckSumAggrResult<int64_t>(aggr_table, DataType::kSmallInt));
        ASSERT_EQ(last_buffer.aggr_cnt_, 1);
        ASSERT_EQ(last_buffer.aggr_val_.vlong, 100);
        ASSERT_EQ(last_buffer.binlog_offset_, 100);
        counter += 2;
        ASSERT_TRUE(GetUpdatedResult(counter, "col5", "sum", "2h", &last_buffer, aggr_table));
        ASSERT_TRUE(CheckSumAggrResult<int64_t>(aggr_table, DataType::kBigInt));
        ASSERT_EQ(last_buffer.aggr_cnt_, 1);
        ASSERT_EQ(last_buffer.aggr_val_.vlong, 100);
        ASSERT_EQ(last_buffer.binlog_offset_, 100);
        counter += 2;
        ASSERT_TRUE(GetUpdatedResult(counter, "col6", "sum", "3h", &last_buffer, aggr_table));
        ASSERT_TRUE(CheckSumAggrResult<float>(aggr_table, DataType::kFloat));
        ASSERT_EQ(last_buffer.aggr_cnt_, 1);
        ASSERT_EQ(last_buffer.aggr_val_.vfloat, static_cast<float>(100));
        ASSERT_EQ(last_buffer.binlog_offset_, 100);
        counter += 2;
        ASSERT_TRUE(GetUpdatedResult(counter, "col7", "sum", "1d", &last_buffer, aggr_table));
        ASSERT_TRUE(CheckSumAggrResult<double>(aggr_table, DataType::kDouble));
        ASSERT_EQ(last_buffer.aggr_cnt_, 1);
        ASSERT_EQ(last_buffer.aggr_val_.vdouble, static_cast<double>(100));
        ASSERT_EQ(last_buffer.binlog_offset_, 100);
    }
    // null data tpye
    {
        uint32_t id = counter++;
        ::openmldb::api::TableMeta base_table_meta;
        base_table_meta.set_tid(id);
        AddDefaultAggregatorBaseSchema(&base_table_meta);
        id = counter++;
        ::openmldb::api::TableMeta aggr_table_meta;
        aggr_table_meta.set_tid(id);
        AddDefaultAggregatorSchema(&aggr_table_meta);
        std::map<std::string, uint32_t> mapping;
        mapping.insert(std::make_pair("idx", 0));
        std::shared_ptr<Table> aggr_table =
            std::make_shared<MemTable>("t", id, 0, 8, mapping, 0, ::openmldb::type::kAbsoluteTime);
        aggr_table->Init();
        auto aggr = CreateAggregator(base_table_meta, aggr_table_meta, aggr_table, 0, "col_null", "sum", "ts_col", "2");
        codec::RowBuilder row_builder(base_table_meta.column_desc());
        ASSERT_TRUE(UpdateAggr(aggr, &row_builder));
        std::string key = "id1|id2";
        ASSERT_EQ(aggr_table->GetRecordCnt(), 50);
        auto it = aggr_table->NewTraverseIterator(0);
        it->SeekToFirst();
        for (int i = 50 - 1; i >= 0; --i) {
            ASSERT_TRUE(it->Valid());
            auto tmp_val = it->GetValue();
            std::string origin_data = tmp_val.ToString();
            codec::RowView origin_row_view(aggr_table_meta.column_desc(),
                                           reinterpret_cast<int8_t*>(const_cast<char*>(origin_data.c_str())),
                                           origin_data.size());
            char* ch = NULL;
            uint32_t ch_length = 0;
            origin_row_view.GetString(4, &ch, &ch_length);
            int32_t val = *reinterpret_cast<int32_t*>(ch);
            ASSERT_EQ(val, 0);
            it->Next();
        }
        AggrBuffer last_buffer;
        auto ok = aggr->GetAggrBuffer(key, &last_buffer);
        ASSERT_TRUE(ok);
        ASSERT_EQ(last_buffer.aggr_cnt_, 1);
        ASSERT_EQ(last_buffer.aggr_val_.vlong, 0);
        ASSERT_EQ(last_buffer.binlog_offset_, 100);
    }
}

TEST_F(AggregatorTest, MinAggregatorUpdate) {
    
    AggrBuffer last_buffer;
    std::shared_ptr<Table> aggr_table;
    ASSERT_TRUE(GetUpdatedResult(counter, "col3", "MIN", "1s", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckMinAggrResult<int32_t>(aggr_table, DataType::kInt));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vsmallint, 100);
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col4", "min", "1m", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckMinAggrResult<int16_t>(aggr_table, DataType::kSmallInt));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vint, 100);
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col5", "min", "2h", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckMinAggrResult<int64_t>(aggr_table, DataType::kBigInt));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vlong, 100);
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col6", "min", "3h", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckMinAggrResult<float>(aggr_table, DataType::kFloat));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vfloat, static_cast<float>(100));
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col7", "min", "1d", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckMinAggrResult<double>(aggr_table, DataType::kDouble));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vdouble, static_cast<double>(100));
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col8", "min", "2d", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckMinAggrResult<int32_t>(aggr_table, DataType::kDate));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vint, static_cast<int32_t>(100));
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    ASSERT_TRUE(GetUpdatedResult(counter, "col9", "min", "2d", &last_buffer, aggr_table));
    ASSERT_EQ(aggr_table->GetRecordCnt(), 50);
    auto it = aggr_table->NewTraverseIterator(0);
    it->SeekToFirst();
    for (int i = 50 - 1; i >= 0; --i) {
        ASSERT_TRUE(it->Valid());
        auto tmp_val = it->GetValue();
        std::string origin_data = tmp_val.ToString();
        codec::RowView origin_row_view(aggr_table->GetTableMeta()->column_desc(),
                                       reinterpret_cast<int8_t*>(const_cast<char*>(origin_data.c_str())),
                                       origin_data.size());
        char* ch = NULL;
        uint32_t ch_length = 0;
        origin_row_view.GetString(4, &ch, &ch_length);
        ASSERT_EQ(strcmp(ch, "abc"), 0);
        it->Next();
    }
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.str_buf, "abc");
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
}

TEST_F(AggregatorTest, MaxAggregatorUpdate) {
    AggrBuffer last_buffer;
    std::shared_ptr<Table> aggr_table;
    ASSERT_TRUE(GetUpdatedResult(counter, "col3", "MAX", "1s", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckMaxAggrResult<int32_t>(aggr_table, DataType::kInt));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vsmallint, 100);
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col4", "Max", "1m", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckMaxAggrResult<int16_t>(aggr_table, DataType::kSmallInt));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vint, 100);
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col5", "max", "2h", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckMaxAggrResult<int64_t>(aggr_table, DataType::kBigInt));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vlong, 100);
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col6", "max", "3h", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckMaxAggrResult<float>(aggr_table, DataType::kFloat));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vfloat, static_cast<float>(100));
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col7", "max", "1d", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckMaxAggrResult<double>(aggr_table, DataType::kDouble));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vdouble, static_cast<double>(100));
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col8", "max", "2d", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckMaxAggrResult<int32_t>(aggr_table, DataType::kDate));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vint, static_cast<int32_t>(100));
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    ASSERT_TRUE(GetUpdatedResult(counter, "col9", "max", "2d", &last_buffer, aggr_table));
    ASSERT_EQ(aggr_table->GetRecordCnt(), 50);
    auto it = aggr_table->NewTraverseIterator(0);
    it->SeekToFirst();
    for (int i = 50 - 1; i >= 0; --i) {
        ASSERT_TRUE(it->Valid());
        auto tmp_val = it->GetValue();
        std::string origin_data = tmp_val.ToString();
        codec::RowView origin_row_view(aggr_table->GetTableMeta()->column_desc(),
                                       reinterpret_cast<int8_t*>(const_cast<char*>(origin_data.c_str())),
                                       origin_data.size());
        char* ch = NULL;
        uint32_t ch_length = 0;
        origin_row_view.GetString(4, &ch, &ch_length);
        ASSERT_EQ(strcmp(ch, "abc"), 0);
        it->Next();
    }
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.str_buf, "abc");
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
}

TEST_F(AggregatorTest, CountAggregatorUpdate) {
    AggrBuffer last_buffer;
    std::shared_ptr<Table> aggr_table;
    ASSERT_TRUE(GetUpdatedResult(counter, "col3", "count", "1s", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckCountAggrResult(aggr_table, DataType::kInt, 2));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.non_null_cnt, 1);
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col_null", "COUNT", "1m", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckCountAggrResult(aggr_table, DataType::kInt, 0));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.non_null_cnt, 0);
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
}

TEST_F(AggregatorTest, AvgAggregatorUpdate) {
    
    AggrBuffer last_buffer;
    std::shared_ptr<Table> aggr_table;
    ASSERT_TRUE(GetUpdatedResult(counter, "col3", "AVG", "1s", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckAvgAggrResult<int64_t>(aggr_table, DataType::kInt));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vlong, 100);
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col4", "Avg", "1m", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckAvgAggrResult<int64_t>(aggr_table, DataType::kSmallInt));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vlong, 100);
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col5", "avg", "2h", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckAvgAggrResult<int64_t>(aggr_table, DataType::kBigInt));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vlong, 100);
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col6", "avg", "3h", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckAvgAggrResult<float>(aggr_table, DataType::kFloat));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vfloat, static_cast<float>(100));
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
    counter += 2;
    ASSERT_TRUE(GetUpdatedResult(counter, "col7", "avg", "1d", &last_buffer, aggr_table));
    ASSERT_TRUE(CheckAvgAggrResult<double>(aggr_table, DataType::kDouble));
    ASSERT_EQ(last_buffer.aggr_cnt_, 1);
    ASSERT_EQ(last_buffer.aggr_val_.vdouble, static_cast<double>(100));
    ASSERT_EQ(last_buffer.binlog_offset_, 100);
}

TEST_F(AggregatorTest, OutOfOrder) {
    uint32_t id = counter++;
    ::openmldb::api::TableMeta base_table_meta;
    base_table_meta.set_tid(id);
    AddDefaultAggregatorBaseSchema(&base_table_meta);
    id = counter++;
    ::openmldb::api::TableMeta aggr_table_meta;
    aggr_table_meta.set_tid(id);
    AddDefaultAggregatorSchema(&aggr_table_meta);
    std::shared_ptr<Table> aggr_table = std::make_shared<MemTable>(aggr_table_meta);
    aggr_table->Init();
    auto aggr = CreateAggregator(base_table_meta, aggr_table_meta, aggr_table, 0, "col3", "sum", "ts_col", "1s");
    codec::RowBuilder row_builder(base_table_meta.column_desc());
    std::string encoded_row;
    uint32_t row_size = row_builder.CalTotalLength(6);
    encoded_row.resize(row_size);
    ASSERT_TRUE(UpdateAggr(aggr, &row_builder));
    std::string key = "id1|id2";
    ASSERT_EQ(aggr_table->GetRecordCnt(), 50);
    // out of order update
    row_builder.SetBuffer(reinterpret_cast<int8_t*>(&(encoded_row[0])), row_size);
    row_builder.AppendString("id1", 3);
    row_builder.AppendString("id2", 3);
    row_builder.AppendTimestamp(static_cast<int64_t>(25) * 1000);
    row_builder.AppendInt32(100);
    row_builder.AppendInt16(100);
    row_builder.AppendInt64(100);
    row_builder.AppendFloat(static_cast<float>(4));
    row_builder.AppendDouble(static_cast<double>(5));
    row_builder.AppendDate(100);
    row_builder.AppendString("abc", 3);
    row_builder.AppendNULL();
    bool ok = aggr->Update(key, encoded_row, 101);
    ASSERT_TRUE(ok);
    ASSERT_EQ(aggr_table->GetRecordCnt(), 51);
    auto it = aggr_table->NewTraverseIterator(0);
    it->Seek(key, 25 * 1000 + 100);
    if (it->Valid()) {
        auto val = it->GetValue();
        std::string origin_data = val.ToString();
        codec::RowView origin_row_view(aggr_table_meta.column_desc(),
                                       reinterpret_cast<int8_t*>(const_cast<char*>(origin_data.c_str())),
                                       origin_data.size());
        int32_t origin_cnt = 0;
        char* ch = NULL;
        uint32_t ch_length = 0;
        origin_row_view.GetInt32(3, &origin_cnt);
        origin_row_view.GetString(4, &ch, &ch_length);
        ASSERT_EQ(origin_cnt, 3);
        int32_t update_val = *reinterpret_cast<int32_t*>(ch);
        ASSERT_EQ(update_val, 201);
    }
}

}  // namespace storage
}  // namespace openmldb

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
