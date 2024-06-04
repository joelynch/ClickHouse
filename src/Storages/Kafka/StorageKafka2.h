#pragma once

#include <Core/BackgroundSchedulePool.h>
#include <Core/Types.h>
#include <Storages/IStorage.h>
#include <Storages/Kafka/KafkaConsumer2.h>
#include <Storages/Kafka/KafkaSettings.h>
#include <Common/ThreadStatus.h>
#include <Common/Macros.h>
#include <Common/SettingsChanges.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Core/Block.h>

#include <Poco/Semaphore.h>

#include <atomic>
#include <list>
#include <mutex>
#include <rdkafka.h>

namespace cppkafka
{

class Configuration;

}

namespace DB
{

template <typename TStorageKafka>
struct StorageKafkaInterceptors;

using KafkaConsumer2Ptr = std::shared_ptr<KafkaConsumer2>;

/** Implements a Kafka queue table engine that can be used as a persistent queue / buffer,
  * or as a basic building block for creating pipelines with a continuous insertion / ETL.
  */
class StorageKafka2 final : public IStorage, WithContext
{
    using StorageKafkaInterceptors = StorageKafkaInterceptors<StorageKafka2>;
    friend StorageKafkaInterceptors;

public:
    StorageKafka2(
        const StorageID & table_id_,
        ContextPtr context_,
        const ColumnsDescription & columns_,
        std::unique_ptr<KafkaSettings> kafka_settings_,
        const String & collection_name_);

    std::string getName() const override { return "Kafka"; }

    bool noPushingToViews() const override { return true; }

    void startup() override;
    void shutdown(bool is_drop) override;

    void drop() override;

    Pipe read(
        const Names & column_names,
        const StorageSnapshotPtr & storage_snapshot,
        SelectQueryInfo & query_info,
        ContextPtr context,
        QueryProcessingStage::Enum processed_stage,
        size_t max_block_size,
        size_t num_streams) override;

    SinkToStoragePtr
    write(const ASTPtr & query, const StorageMetadataPtr & /*metadata_snapshot*/, ContextPtr context, bool async_insert) override;

    /// We want to control the number of rows in a chunk inserted into Kafka
    bool prefersLargeBlocks() const override { return false; }

    const auto & getFormatName() const { return format_name; }

    StreamingHandleErrorMode getHandleKafkaErrorMode() const { return kafka_settings->kafka_handle_error_mode; }

private:
    using TopicPartition = KafkaConsumer2::TopicPartition;
    using TopicPartitions = KafkaConsumer2::TopicPartitions;

    struct LockedTopicPartitionInfo
    {
        zkutil::EphemeralNodeHolderPtr lock;
        std::optional<int64_t> committed_offset;
        std::optional<int64_t> intent_size;
    };

    using TopicPartitionLocks = std::unordered_map<
        TopicPartition,
        LockedTopicPartitionInfo,
        KafkaConsumer2::OnlyTopicNameAndPartitionIdHash,
        KafkaConsumer2::OnlyTopicNameAndPartitionIdEquality>;

    struct ConsumerAndAssignmentInfo
    {
        KafkaConsumer2Ptr consumer; /// available consumers
        size_t consume_from_topic_partition_index{0};
        TopicPartitions topic_partitions;
        // TODO(antaljanosbenjamin): maybe recreate the ephemeral node
        zkutil::ZooKeeperPtr keeper;
        TopicPartitionLocks locks;
    };

    struct PolledBatchInfo
    {
        BlocksList blocks;
        int64_t last_offset;
    };

    // Stream thread
    struct TaskContext
    {
        BackgroundSchedulePool::TaskHolder holder;
        std::atomic<bool> stream_cancelled{false};
        explicit TaskContext(BackgroundSchedulePool::TaskHolder && task_) : holder(std::move(task_)) { }
    };

    enum class AssignmentChange
    {
        NotChanged,
        Updated,
        Lost
    };

    // Configuration and state
    std::mutex keeper_mutex;
    zkutil::ZooKeeperPtr keeper;
    std::unique_ptr<KafkaSettings> kafka_settings;
    Macros::MacroExpansionInfo macros_info;
    const Names topics;
    const String brokers;
    const String group;
    const String client_id;
    const String format_name;
    const size_t max_rows_per_message;
    const String schema_name;
    const size_t num_consumers; /// total number of consumers
    LoggerPtr log;
    Poco::Semaphore semaphore;
    const SettingsChanges settings_adjustments;
    std::atomic<bool> mv_attached = false;
    /// Can differ from num_consumers in case of exception in startup() (or if startup() hasn't been called).
    /// In this case we still need to be able to shutdown() properly.
    size_t num_created_consumers = 0; /// number of actually created consumers.
    std::vector<ConsumerAndAssignmentInfo> consumers;
    std::vector<std::shared_ptr<TaskContext>> tasks;
    bool thread_per_consumer = false;
    /// For memory accounting in the librdkafka threads.
    std::mutex thread_statuses_mutex;
    std::list<std::shared_ptr<ThreadStatus>> thread_statuses;
    /// If named_collection is specified.
    String collection_name;
    std::atomic<bool> shutdown_called = false;

    SettingsChanges createSettingsAdjustments();
    KafkaConsumer2Ptr createConsumer(size_t consumer_number);
    // Returns full consumer related configuration, also the configuration
    // contains global kafka properties.
    cppkafka::Configuration getConsumerConfiguration(size_t consumer_number);
    // Returns full producer related configuration, also the configuration
    // contains global kafka properties.
    cppkafka::Configuration getProducerConfiguration();
    // Load Kafka global configuration
    // https://github.com/confluentinc/librdkafka/blob/master/CONFIGURATION.md#global-configuration-properties
    void updateGlobalConfiguration(cppkafka::Configuration & kafka_config);
    // Load Kafka properties from consumer configuration
    // NOTE: librdkafka allow to set a consumer property to a producer and vice versa,
    //       but a warning will be generated e.g:
    //       "Configuration property session.timeout.ms is a consumer property and
    //        will be ignored by this producer instance"
    void updateConsumerConfiguration(cppkafka::Configuration & kafka_config);
    // Load Kafka properties from producer configuration
    void updateProducerConfiguration(cppkafka::Configuration & kafka_config);

    String getConfigPrefix() const;
    void threadFunc(size_t idx);

    size_t getPollMaxBatchSize() const;
    size_t getMaxBlockSize() const;
    size_t getPollTimeoutMillisecond() const;

    static Names parseTopics(String topic_list);
    static String getDefaultClientId(const StorageID & table_id_);

    bool streamToViews(size_t idx);

    std::optional<size_t> streamFromConsumer(ConsumerAndAssignmentInfo & consumer_info);

    bool checkDependencies(const StorageID & table_id);

    // Returns true if this is the first replica
    bool createTableIfNotExists(const KafkaConsumer2Ptr & consumer);
    // Returns true if all of the nodes were cleaned up
    bool removeTableNodesFromZooKeeper(const zkutil::EphemeralNodeHolder::Ptr & drop_lock);
    // Creates only the replica in ZooKeeper. Shouldn't be called on the first replica as it is created in createTableIfNotExists
    void createReplica();
    void dropReplica();

    // Takes lock over topic partitions and set's the committed offset in topic_partitions
    void createKeeperNodes(const KafkaConsumer2Ptr & consumer);

    std::optional<TopicPartitionLocks> lockTopicPartitions(zkutil::ZooKeeper& keeper_to_use, const TopicPartitions & topic_partitions);
    void saveCommittedOffset(zkutil::ZooKeeper& keeper_to_use,const TopicPartition & topic_partition, int64_t committed_offset);
    void saveIntent(zkutil::ZooKeeper& keeper_to_use,const TopicPartition & topic_partition, int64_t intent);

    PolledBatchInfo pollConsumer(
        KafkaConsumer2 & consumer,
        const TopicPartition & topic_partition,
        std::optional<int64_t> message_count,
        const ContextPtr & context);

    zkutil::ZooKeeperPtr getZooKeeper();

    std::string getTopicPartitionPath(const TopicPartition & topic_partition);

    static VirtualColumnsDescription createVirtuals(StreamingHandleErrorMode handle_error_mode);
};

}
