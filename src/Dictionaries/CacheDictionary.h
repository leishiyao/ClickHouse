#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <variant>
#include <vector>
#include <common/logger_useful.h>
#include <Columns/ColumnDecimal.h>
#include <Columns/ColumnString.h>
#include <Common/ThreadPool.h>
#include <Common/ConcurrentBoundedQueue.h>
#include <pcg_random.hpp>
#include <Common/ArenaWithFreeLists.h>
#include <Common/CurrentMetrics.h>
#include <ext/bit_cast.h>
#include "DictionaryStructure.h"
#include "IDictionary.h"
#include "IDictionarySource.h"

namespace CurrentMetrics
{
    extern const Metric CacheDictionaryUpdateQueueBatches;
    extern const Metric CacheDictionaryUpdateQueueKeys;
}


namespace DB
{

namespace ErrorCodes
{
}

/*
 *
 * This dictionary is stored in a cache that has a fixed number of cells.
 * These cells contain frequently used elements.
 * When searching for a dictionary, the cache is searched first and special heuristic is used:
 * while looking for the key, we take a look only at max_collision_length elements.
 * So, our cache is not perfect. It has errors like "the key is in cache, but the cache says that it does not".
 * And in this case we simply ask external source for the key which is faster.
 * You have to keep this logic in mind.
 * */
class CacheDictionary final : public IDictionary
{
public:
    CacheDictionary(
        const StorageID & dict_id_,
        const DictionaryStructure & dict_struct_,
        DictionarySourcePtr source_ptr_,
        DictionaryLifetime dict_lifetime_,
        size_t strict_max_lifetime_seconds,
        size_t size_,
        bool allow_read_expired_keys_,
        size_t max_update_queue_size_,
        size_t update_queue_push_timeout_milliseconds_,
        size_t query_wait_timeout_milliseconds,
        size_t max_threads_for_updates);

    ~CacheDictionary() override;

    std::string getTypeName() const override { return "Cache"; }

    size_t getBytesAllocated() const override;

    size_t getQueryCount() const override { return query_count.load(std::memory_order_relaxed); }

    double getHitRate() const override
    {
        return static_cast<double>(hit_count.load(std::memory_order_acquire)) / query_count.load(std::memory_order_relaxed);
    }

    size_t getElementCount() const override { return element_count.load(std::memory_order_relaxed); }

    double getLoadFactor() const override { return static_cast<double>(element_count.load(std::memory_order_relaxed)) / size; }

    bool supportUpdates() const override { return false; }

    std::shared_ptr<const IExternalLoadable> clone() const override
    {
        return std::make_shared<CacheDictionary>(
                getDictionaryID(),
                dict_struct,
                getSourceAndUpdateIfNeeded()->clone(),
                dict_lifetime,
                strict_max_lifetime_seconds,
                size,
                allow_read_expired_keys,
                max_update_queue_size,
                update_queue_push_timeout_milliseconds,
                query_wait_timeout_milliseconds,
                max_threads_for_updates);
    }

    const IDictionarySource * getSource() const override;

    const DictionaryLifetime & getLifetime() const override { return dict_lifetime; }

    const DictionaryStructure & getStructure() const override { return dict_struct; }

    bool isInjective(const std::string & attribute_name) const override
    {
        return dict_struct.attributes[&getAttribute(attribute_name) - attributes.data()].injective;
    }

    bool hasHierarchy() const override { return hierarchical_attribute; }

    void toParent(const PaddedPODArray<Key> & ids, PaddedPODArray<Key> & out) const override;

    void isInVectorVector(
        const PaddedPODArray<Key> & child_ids, const PaddedPODArray<Key> & ancestor_ids, PaddedPODArray<UInt8> & out) const override;
    void isInVectorConstant(const PaddedPODArray<Key> & child_ids, const Key ancestor_id, PaddedPODArray<UInt8> & out) const override;
    void isInConstantVector(const Key child_id, const PaddedPODArray<Key> & ancestor_ids, PaddedPODArray<UInt8> & out) const override;

    std::exception_ptr getLastException() const override;

    template <typename T>
    using ResultArrayType = std::conditional_t<IsDecimalNumber<T>, DecimalPaddedPODArray<T>, PaddedPODArray<T>>;

#define DECLARE(TYPE) \
    void get##TYPE(const std::string & attribute_name, const PaddedPODArray<Key> & ids, ResultArrayType<TYPE> & out) const;
    DECLARE(UInt8)
    DECLARE(UInt16)
    DECLARE(UInt32)
    DECLARE(UInt64)
    DECLARE(UInt128)
    DECLARE(Int8)
    DECLARE(Int16)
    DECLARE(Int32)
    DECLARE(Int64)
    DECLARE(Float32)
    DECLARE(Float64)
    DECLARE(Decimal32)
    DECLARE(Decimal64)
    DECLARE(Decimal128)
#undef DECLARE

    void getString(const std::string & attribute_name, const PaddedPODArray<Key> & ids, ColumnString * out) const;

#define DECLARE(TYPE) \
    void get##TYPE( \
        const std::string & attribute_name, \
        const PaddedPODArray<Key> & ids, \
        const PaddedPODArray<TYPE> & def, \
        ResultArrayType<TYPE> & out) const;
    DECLARE(UInt8)
    DECLARE(UInt16)
    DECLARE(UInt32)
    DECLARE(UInt64)
    DECLARE(UInt128)
    DECLARE(Int8)
    DECLARE(Int16)
    DECLARE(Int32)
    DECLARE(Int64)
    DECLARE(Float32)
    DECLARE(Float64)
    DECLARE(Decimal32)
    DECLARE(Decimal64)
    DECLARE(Decimal128)
#undef DECLARE

    void
    getString(const std::string & attribute_name, const PaddedPODArray<Key> & ids, const ColumnString * const def, ColumnString * const out)
        const;

#define DECLARE(TYPE) \
    void get##TYPE(const std::string & attribute_name, const PaddedPODArray<Key> & ids, const TYPE def, ResultArrayType<TYPE> & out) const;
    DECLARE(UInt8)
    DECLARE(UInt16)
    DECLARE(UInt32)
    DECLARE(UInt64)
    DECLARE(UInt128)
    DECLARE(Int8)
    DECLARE(Int16)
    DECLARE(Int32)
    DECLARE(Int64)
    DECLARE(Float32)
    DECLARE(Float64)
    DECLARE(Decimal32)
    DECLARE(Decimal64)
    DECLARE(Decimal128)
#undef DECLARE

    void getString(const std::string & attribute_name, const PaddedPODArray<Key> & ids, const String & def, ColumnString * const out) const;

    void has(const PaddedPODArray<Key> & ids, PaddedPODArray<UInt8> & out) const override;

    BlockInputStreamPtr getBlockInputStream(const Names & column_names, size_t max_block_size) const override;

private:
    template <typename Value>
    using ContainerType = Value[];
    template <typename Value>
    using ContainerPtrType = std::unique_ptr<ContainerType<Value>>;

    struct CellMetadata final
    {
        using time_point_t = std::chrono::system_clock::time_point;
        using time_point_rep_t = time_point_t::rep;
        using time_point_urep_t = std::make_unsigned_t<time_point_rep_t>;

        static constexpr UInt64 EXPIRES_AT_MASK = std::numeric_limits<time_point_rep_t>::max();
        static constexpr UInt64 IS_DEFAULT_MASK = ~EXPIRES_AT_MASK;

        UInt64 id;
        /// Stores both expiration time and `is_default` flag in the most significant bit
        time_point_urep_t data;

        time_point_t strict_max;

        /// Sets expiration time, resets `is_default` flag to false
        time_point_t expiresAt() const { return ext::safe_bit_cast<time_point_t>(data & EXPIRES_AT_MASK); }
        void setExpiresAt(const time_point_t & t) { data = ext::safe_bit_cast<time_point_urep_t>(t); }

        bool isDefault() const { return (data & IS_DEFAULT_MASK) == IS_DEFAULT_MASK; }
        void setDefault() { data |= IS_DEFAULT_MASK; }
    };

    struct Attribute final
    {
        AttributeUnderlyingType type;
        std::variant<
            UInt8,
            UInt16,
            UInt32,
            UInt64,
            UInt128,
            Int8,
            Int16,
            Int32,
            Int64,
            Decimal32,
            Decimal64,
            Decimal128,
            Float32,
            Float64,
            String>
            null_values;
        std::variant<
            ContainerPtrType<UInt8>,
            ContainerPtrType<UInt16>,
            ContainerPtrType<UInt32>,
            ContainerPtrType<UInt64>,
            ContainerPtrType<UInt128>,
            ContainerPtrType<Int8>,
            ContainerPtrType<Int16>,
            ContainerPtrType<Int32>,
            ContainerPtrType<Int64>,
            ContainerPtrType<Decimal32>,
            ContainerPtrType<Decimal64>,
            ContainerPtrType<Decimal128>,
            ContainerPtrType<Float32>,
            ContainerPtrType<Float64>,
            ContainerPtrType<StringRef>>
            arrays;
    };

    void createAttributes();

    Attribute createAttributeWithType(const AttributeUnderlyingType type, const Field & null_value);

    template <typename AttributeType, typename OutputType, typename DefaultGetter>
    void getItemsNumberImpl(
        Attribute & attribute, const PaddedPODArray<Key> & ids, ResultArrayType<OutputType> & out, DefaultGetter && get_default) const;

    template <typename DefaultGetter>
    void getItemsString(Attribute & attribute, const PaddedPODArray<Key> & ids, ColumnString * out, DefaultGetter && get_default) const;

    PaddedPODArray<Key> getCachedIds() const;

    bool isEmptyCell(const UInt64 idx) const;

    size_t getCellIdx(const Key id) const;

    void setDefaultAttributeValue(Attribute & attribute, const Key idx) const;

    void setAttributeValue(Attribute & attribute, const Key idx, const Field & value) const;

    Attribute & getAttribute(const std::string & attribute_name) const;

    using SharedDictionarySourcePtr = std::shared_ptr<IDictionarySource>;

    /// Update dictionary source pointer if required and return it. Thread safe.
    /// MultiVersion is not used here because it works with constant pointers.
    /// For some reason almost all methods in IDictionarySource interface are
    /// not constant.
    SharedDictionarySourcePtr getSourceAndUpdateIfNeeded() const
    {
        std::lock_guard lock(source_mutex);
        if (error_count)
        {
            /// Recover after error: we have to clone the source here because
            /// it could keep connections which should be reset after error.
            auto new_source_ptr = source_ptr->clone();
            source_ptr = std::move(new_source_ptr);
        }

        return source_ptr;
    }

    struct FindResult
    {
        const size_t cell_idx;
        const bool valid;
        const bool outdated;
    };

    FindResult findCellIdx(const Key & id, const CellMetadata::time_point_t now) const;

    template <typename AncestorType>
    void isInImpl(const PaddedPODArray<Key> & child_ids, const AncestorType & ancestor_ids, PaddedPODArray<UInt8> & out) const;

    const DictionaryStructure dict_struct;

    /// Dictionary source should be used with mutex
    mutable std::mutex source_mutex;
    mutable SharedDictionarySourcePtr source_ptr;

    const DictionaryLifetime dict_lifetime;
    const size_t strict_max_lifetime_seconds;
    const bool allow_read_expired_keys;
    const size_t max_update_queue_size;
    const size_t update_queue_push_timeout_milliseconds;
    const size_t query_wait_timeout_milliseconds;
    const size_t max_threads_for_updates;

    Poco::Logger * log;

    /// This lock is used for the inner cache state update function lock it for
    /// write, when it need to update cache state all other functions just
    /// readers. Surprisingly this lock is also used for last_exception pointer.
    mutable std::shared_mutex rw_lock;

    /// Actual size will be increased to match power of 2
    const size_t size;

    /// all bits to 1  mask (size - 1) (0b1000 - 1 = 0b111)
    const size_t size_overlap_mask;

    /// Max tries to find cell, overlapped with mask: if size = 16 and start_cell=10: will try cells: 10,11,12,13,14,15,0,1,2,3
    static constexpr size_t max_collision_length = 10;

    const size_t zero_cell_idx{getCellIdx(0)};
    std::map<std::string, size_t> attribute_index_by_name;
    mutable std::vector<Attribute> attributes;
    mutable std::vector<CellMetadata> cells;
    Attribute * hierarchical_attribute = nullptr;
    std::unique_ptr<ArenaWithFreeLists> string_arena;

    mutable std::exception_ptr last_exception;
    mutable std::atomic<size_t> error_count = 0;
    mutable std::atomic<std::chrono::system_clock::time_point> backoff_end_time{std::chrono::system_clock::time_point{}};

    mutable pcg64 rnd_engine;

    mutable size_t bytes_allocated = 0;
    mutable std::atomic<size_t> element_count{0};
    mutable std::atomic<size_t> hit_count{0};
    mutable std::atomic<size_t> query_count{0};

    /// Field and methods correlated with update expired and not found keys

    using PresentIdHandler = std::function<void(Key, size_t)>;
    using AbsentIdHandler  = std::function<void(Key, size_t)>;

    /*
     * Disclaimer: this comment is written not for fun.
     *
     * How the update goes: we basically have a method like get(keys)->values. Values are cached, so sometimes we
     * can return them from the cache. For values not in cache, we query them from the dictionary, and add to the
     * cache. The cache is lossy, so we can't expect it to store all the keys, and we store them separately. Normally,
     * they would be passed as a return value of get(), but for Unknown Reasons the dictionaries use a baroque
     * interface where get() accepts two callback, one that it calls for found values, and one for not found.
     *
     * Now we make it even uglier by doing this from multiple threads. The missing values are retrieved from the
     * dictionary in a background thread, and this thread calls the provided callback. So if you provide the callbacks,
     * you MUST wait until the background update finishes, or god knows what happens. Unfortunately, we have no
     * way to check that you did this right, so good luck.
     */
    struct UpdateUnit
    {
        UpdateUnit(std::vector<Key> requested_ids_,
                PresentIdHandler present_id_handler_,
                AbsentIdHandler absent_id_handler_) :
                requested_ids(std::move(requested_ids_)),
                alive_keys(CurrentMetrics::CacheDictionaryUpdateQueueKeys, requested_ids.size()),
                present_id_handler(present_id_handler_),
                absent_id_handler(absent_id_handler_){}

        explicit UpdateUnit(std::vector<Key> requested_ids_) :
                requested_ids(std::move(requested_ids_)),
                alive_keys(CurrentMetrics::CacheDictionaryUpdateQueueKeys, requested_ids.size()),
                present_id_handler([](Key, size_t){}),
                absent_id_handler([](Key, size_t){}){}


        void callPresentIdHandler(Key key, size_t cell_idx)
        {
            std::lock_guard lock(callback_mutex);
            if (can_use_callback)
                present_id_handler(key, cell_idx);
        }

        void callAbsentIdHandler(Key key, size_t cell_idx)
        {
            std::lock_guard lock(callback_mutex);
            if (can_use_callback)
                absent_id_handler(key, cell_idx);
        }

        std::vector<Key> requested_ids;

        /// It might seem that it is a leak of performance.
        /// But acquiring a mutex without contention is rather cheap.
        std::mutex callback_mutex;
        bool can_use_callback{true};

        std::atomic<bool> is_done{false};
        std::exception_ptr current_exception{nullptr};

        /// While UpdateUnit is alive, it is accounted in update_queue size.
        CurrentMetrics::Increment alive_batch{CurrentMetrics::CacheDictionaryUpdateQueueBatches};
        CurrentMetrics::Increment alive_keys;

      private:
        PresentIdHandler present_id_handler;
        AbsentIdHandler absent_id_handler;
    };

    using UpdateUnitPtr = std::shared_ptr<UpdateUnit>;
    using UpdateQueue = ConcurrentBoundedQueue<UpdateUnitPtr>;

    mutable UpdateQueue update_queue;

    ThreadPool update_pool;

    /*
     *  Actually, we can divide all requested keys into two 'buckets'. There are only four possible states and they
     * are described in the table.
     *
     * cache_not_found_ids  |0|0|1|1|
     * cache_expired_ids    |0|1|0|1|
     *
     * 0 - if set is empty, 1 - otherwise
     *
     * Only if there are no cache_not_found_ids and some cache_expired_ids
     * (with allow_read_expired_keys_from_cache_dictionary setting) we can perform async update.
     * Otherwise we have no concatenate ids and update them sync.
     *
     */
    void updateThreadFunction();
    void update(UpdateUnitPtr & update_unit_ptr) const;


    void tryPushToUpdateQueueOrThrow(UpdateUnitPtr & update_unit_ptr) const;
    void waitForCurrentUpdateFinish(UpdateUnitPtr & update_unit_ptr) const;

    mutable std::mutex update_mutex;
    mutable std::condition_variable is_update_finished;

    std::atomic<bool> finished{false};
    };
}
