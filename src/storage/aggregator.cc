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

#include "boost/algorithm/string.hpp"

#include "base/glog_wapper.h"
#include "base/strings.h"
#include "common/timer.h"
#include "storage/aggregator.h"
#include "storage/table.h"

namespace openmldb {
namespace storage {

Aggregator::Aggregator(const ::openmldb::api::TableMeta& base_meta, const ::openmldb::api::TableMeta& aggr_meta,
                       std::shared_ptr<Table> aggr_table, const uint32_t& index_pos, const std::string& aggr_col,
                       const AggrType& aggr_type, const std::string& ts_col, WindowType window_tpye,
                       uint32_t window_size)
    : base_table_schema_(base_meta.column_desc()),
      aggr_table_schema_(aggr_meta.column_desc()),
      aggr_table_(aggr_table),
      index_pos_(index_pos),
      aggr_col_(aggr_col),
      aggr_type_(aggr_type),
      ts_col_(ts_col),
      window_type_(window_tpye),
      window_size_(window_size),
      base_row_view_(base_table_schema_),
      aggr_row_view_(aggr_table_schema_),
      row_builder_(aggr_table_schema_) {
    for (int i = 0; i < base_meta.column_desc().size(); i++) {
        if (base_meta.column_desc(i).name() == aggr_col_) {
            aggr_col_idx_ = i;
        }
        if (base_meta.column_desc(i).name() == ts_col_) {
            ts_col_idx_ = i;
        }
    }
    aggr_col_type_ = base_meta.column_desc(aggr_col_idx_).data_type();
    ts_col_type_ = base_meta.column_desc(ts_col_idx_).data_type();
    auto dimension = dimensions_.Add();
    dimension->set_idx(0);
}

bool Aggregator::Update(const std::string& key, const std::string& row, const uint64_t& offset) {
    int8_t* row_ptr = reinterpret_cast<int8_t*>(const_cast<char*>(row.c_str()));
    int64_t cur_ts;
    switch (ts_col_type_) {
        case DataType::kBigInt: {
            base_row_view_.GetValue(row_ptr, ts_col_idx_, DataType::kBigInt, &cur_ts);
            break;
        }
        case DataType::kTimestamp: {
            base_row_view_.GetValue(row_ptr, ts_col_idx_, DataType::kTimestamp, &cur_ts);
            break;
        }
        default: {
            LOG(ERROR) << "Unsupported timestamp type: " << ts_col_type_;
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        if (aggr_buffer_map_.find(key) == aggr_buffer_map_.end()) {
            aggr_buffer_map_.emplace(key, AggrBufferLocked{});
        }
    }

    auto& aggr_buffer_lock = aggr_buffer_map_.at(key);
    std::unique_lock<std::mutex> lock(*aggr_buffer_lock.mu_);
    AggrBuffer& aggr_buffer = aggr_buffer_lock.buffer_;
    if (aggr_buffer.ts_begin_ == -1) {
        aggr_buffer.ts_begin_ = cur_ts;
        if (window_type_ == WindowType::kRowsRange) {
            aggr_buffer.ts_end_ = cur_ts + window_size_ - 1;
        }
    }
    if (window_type_ == WindowType::kRowsRange && cur_ts > aggr_buffer.ts_end_) {
        AggrBuffer flush_buffer = aggr_buffer;
        int64_t latest_ts = aggr_buffer.ts_end_ + 1;
        aggr_buffer.clear();
        aggr_buffer.ts_begin_ = latest_ts;
        aggr_buffer.ts_end_ = latest_ts + window_size_ - 1;
        lock.unlock();
        FlushAggrBuffer(key, flush_buffer);
        lock.lock();
    }

    // handle the case that the current timestamp is smaller than the begin timestamp in aggregate buffer
    lock.unlock();
    if (cur_ts < aggr_buffer.ts_begin_) {
        bool ok = UpdateFlushedBuffer(key, row_ptr, cur_ts, offset);
        if (!ok) {
            LOG(ERROR) << "Update flushed buffer failed";
            return false;
        }
    }
    lock.lock();

    aggr_buffer.aggr_cnt_++;
    aggr_buffer.binlog_offset_ = offset;
    if (window_type_ == WindowType::kRowsNum) {
        aggr_buffer.ts_end_ = cur_ts;
    }
    bool ok = UpdateAggrVal(base_row_view_, row_ptr, &aggr_buffer);
    if (!ok) {
        return false;
    }

    if (window_type_ == WindowType::kRowsNum && aggr_buffer.aggr_cnt_ >= window_size_) {
        AggrBuffer flush_buffer = aggr_buffer;
        int64_t latest_ts = aggr_buffer.ts_end_ + 1;
        aggr_buffer.clear();
        aggr_buffer.ts_begin_ = latest_ts;
        lock.unlock();
        FlushAggrBuffer(key, flush_buffer);
        lock.lock();
    }
    return true;
}

bool Aggregator::GetAggrBufferFromRowView(codec::RowView* row_view, int8_t* row_ptr, AggrBuffer* buffer) {
    if (buffer == nullptr) {
        return false;
    }
    row_view->GetValue(row_ptr, 1, DataType::kTimestamp, &buffer->ts_begin_);
    row_view->GetValue(row_ptr, 2, DataType::kTimestamp, &buffer->ts_end_);
    row_view->GetValue(row_ptr, 3, DataType::kInt, &buffer->aggr_cnt_);
    char* ch = NULL;
    uint32_t ch_length = 0;
    row_view->GetValue(row_ptr, 4, &ch, &ch_length);
    switch (aggr_col_type_) {
        case DataType::kSmallInt:
        case DataType::kInt:
        case DataType::kBigInt: {
            int64_t origin_val = *reinterpret_cast<int64_t*>(ch);
            buffer->aggr_val_.vlong = origin_val;
            break;
        }
        case DataType::kFloat: {
            float origin_val = *reinterpret_cast<float*>(ch);
            buffer->aggr_val_.vfloat = origin_val;
            break;
        }
        case DataType::kDouble: {
            double origin_val = *reinterpret_cast<double*>(ch);
            buffer->aggr_val_.vdouble = origin_val;
            break;
        }
        default: {
            PDLOG(WARNING, "unsupported data type");
            return false;
        }
    }
    return true;
}

bool Aggregator::FlushAggrBuffer(const std::string& key, AggrBuffer buffer) {
    std::string encoded_row;
    std::string aggr_val;
    switch (aggr_col_type_) {
        case DataType::kSmallInt:
        case DataType::kInt:
        case DataType::kBigInt: {
            int64_t tmp_val = buffer.aggr_val_.vlong;
            aggr_val.assign(reinterpret_cast<char*>(&tmp_val), sizeof(int64_t));
            break;
        }
        case DataType::kFloat: {
            float tmp_val = buffer.aggr_val_.vfloat;
            aggr_val.assign(reinterpret_cast<char*>(&tmp_val), sizeof(float));
            break;
        }
        case DataType::kDouble: {
            double tmp_val = buffer.aggr_val_.vdouble;
            aggr_val.assign(reinterpret_cast<char*>(&tmp_val), sizeof(double));
            break;
        }
        default: {
            PDLOG(WARNING, "unsupported data type");
            return false;
        }
    }

    int str_length = key.size() + aggr_val.size();
    uint32_t row_size = row_builder_.CalTotalLength(str_length);
    encoded_row.resize(row_size);
    {
        std::lock_guard<std::mutex> lock(rb_mu_);
        row_builder_.SetBuffer(reinterpret_cast<int8_t*>(&(encoded_row[0])), row_size);
        row_builder_.AppendString(key.c_str(), key.size());
        row_builder_.AppendTimestamp(buffer.ts_begin_);
        row_builder_.AppendTimestamp(buffer.ts_end_);
        row_builder_.AppendInt32(buffer.aggr_cnt_);
        row_builder_.AppendString(aggr_val.c_str(), aggr_val.size());
        row_builder_.AppendInt64(buffer.binlog_offset_);
    }

    int64_t time = ::baidu::common::timer::get_micros() / 1000;
    dimensions_.Mutable(0)->set_key(key);
    bool ok = aggr_table_->Put(time, encoded_row, dimensions_);
    if (!ok) {
        LOG(ERROR) << "aggregator put failed";
        return false;
    }
    return true;
}

bool Aggregator::UpdateFlushedBuffer(const std::string& key, int8_t* base_row_ptr, int64_t cur_ts, uint64_t offset) {
    auto it = aggr_table_->NewTraverseIterator(0);
    // If there is no repetition of ts, `seek` will locate to the position that less than ts.
    it->Seek(key, cur_ts + 1);
    AggrBuffer tmp_buffer;
    if (it->Valid()) {
        auto val = it->GetValue();
        std::string origin_data = val.ToString();
        int8_t* aggr_row_ptr = reinterpret_cast<int8_t*>(const_cast<char*>(origin_data.c_str()));

        bool ok = GetAggrBufferFromRowView(&aggr_row_view_, aggr_row_ptr, &tmp_buffer);
        if (!ok) {
            LOG(ERROR) << "GetAggrBufferFromRowView failed";
            return false;
        }
        if (cur_ts > tmp_buffer.ts_end_ || cur_ts < tmp_buffer.ts_begin_) {
            LOG(ERROR) << "current ts isn't in buffer range";
            return false;
        }
        tmp_buffer.aggr_cnt_ += 1;
        tmp_buffer.binlog_offset_ = offset;
    } else {
        tmp_buffer.ts_begin_ = cur_ts;
        tmp_buffer.ts_end_ = cur_ts;
        tmp_buffer.aggr_cnt_ = 1;
        tmp_buffer.binlog_offset_ = offset;
    }
    bool ok = UpdateAggrVal(base_row_view_, base_row_ptr, &tmp_buffer);
    if (!ok) {
        LOG(ERROR) << "UpdateAggrVal failed";
        return false;
    }
    ok = FlushAggrBuffer(key, tmp_buffer);
    if (!ok) {
        LOG(ERROR) << "FlushAggrBuffer failed";
        return false;
    }
    return true;
}

SumAggregator::SumAggregator(const ::openmldb::api::TableMeta& base_meta, const ::openmldb::api::TableMeta& aggr_meta,
                             std::shared_ptr<Table> aggr_table, const uint32_t& index_pos, const std::string& aggr_col,
                             const AggrType& aggr_type, const std::string& ts_col, WindowType window_tpye,
                             uint32_t window_size)
    : Aggregator(base_meta, aggr_meta, aggr_table, index_pos, aggr_col, aggr_type, ts_col, window_tpye, window_size) {}

bool SumAggregator::UpdateAggrVal(const codec::RowView& row_view, int8_t* row_ptr, AggrBuffer* aggr_buffer) {
    switch (aggr_col_type_) {
        case DataType::kSmallInt: {
            int16_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vlong += val;
            break;
        }
        case DataType::kInt: {
            int32_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vlong += val;
            break;
        }
        case DataType::kBigInt: {
            int64_t val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vlong += val;
            break;
        }
        case DataType::kFloat:
        case DataType::kDouble: {
            double val;
            row_view.GetValue(row_ptr, aggr_col_idx_, aggr_col_type_, &val);
            aggr_buffer->aggr_val_.vdouble += val;
            break;
        }
        default: {
            PDLOG(WARNING, "unsupported data type");
            return false;
        }
    }
    return true;
}

std::shared_ptr<Aggregator> CreateAggregator(const ::openmldb::api::TableMeta& base_meta,
                                             const ::openmldb::api::TableMeta& aggr_meta,
                                             std::shared_ptr<Table> aggr_table, const uint32_t& index_pos,
                                             const std::string& aggr_col, const std::string& aggr_func,
                                             const std::string& ts_col, const std::string& bucket_size) {
    std::string aggr_type = boost::to_lower_copy(aggr_func);
    WindowType window_type;
    uint32_t window_size;
    if (::openmldb::base::IsNumber(bucket_size)) {
        window_type = WindowType::kRowsNum;
        window_size = std::stoi(bucket_size);
    } else {
        window_type = WindowType::kRowsRange;
        if (bucket_size.empty()) {
            PDLOG(WARNING, "bucket size is empty");
            return std::shared_ptr<Aggregator>();
        }
        char time_unit = bucket_size.back();
        std::string time_size = bucket_size.substr(0, bucket_size.size() - 1);
        if (!::openmldb::base::IsNumber(time_size)) {
            PDLOG(WARNING, "bucket size is not a number");
            return std::shared_ptr<Aggregator>();
        }
        switch (time_unit) {
            case 's':
                window_size = std::stoi(time_size) * 1000;
                break;
            case 'm':
                window_size = std::stoi(time_size) * 1000 * 60;
                break;
            case 'h':
                window_size = std::stoi(time_size) * 1000 * 60 * 60;
                break;
            case 'd':
                window_size = std::stoi(time_size) * 1000 * 60 * 60 * 24;
                break;
            default: {
                PDLOG(WARNING, "Unsupported time unit");
                return std::shared_ptr<Aggregator>();
            }
        }
    }

    // TODO: support more aggr_type
    if (aggr_func == "sum") {
        return std::make_shared<SumAggregator>(base_meta, aggr_meta, aggr_table, index_pos, aggr_col, AggrType::kSum,
                                               ts_col, window_type, window_size);
    } else {
        PDLOG(WARNING, "Unsupported aggregate function type");
        return std::shared_ptr<Aggregator>();
    }
    return std::shared_ptr<Aggregator>();
}

}  // namespace storage
}  // namespace openmldb
