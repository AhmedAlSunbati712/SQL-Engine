#include <gtest/gtest.h>

#include <KeyCodec.h>
#include <LockManager/LockManager.h>
#include <Log/Log.h>
#include <Log/PendingBTreeAction.h>
#include <Log/WalRecords.h>
#include <TransactionManager/TransactionManager.h>
#include <V2PageCodec.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

class TempDir {
public:
    TempDir() {
        path = std::filesystem::temp_directory_path() /
            std::filesystem::path("stoneleaf-transaction-manager-XXXXXX");
        std::string value = path.string();
        value.push_back('\0');
        path = ::mkdtemp(value.data());
    }

    ~TempDir() { std::filesystem::remove_all(path); }

    std::filesystem::path path;
};

Config config() {
    return {
        .max_index_bytes = 100 * Index::ENTRY_SIZE,
        .max_store_bytes = 1024 * 1024,
        .initial_lsn = 1,
    };
}

PageEffect effect(std::uint32_t page_num, char marker) {
    PageEffect result{
        .kind = PageEffectKind::Write,
        .page_num = page_num,
    };
    V2PageCodec::initialize(
        result.after_image,
        page_num,
        V2PageKind::BTreeLeaf);
    result.after_image[V2_PAGE_HEADER_SIZE] = marker;
    return result;
}

class RecordingUndoExecutor final : public TransactionUndoExecutor {
public:
    std::vector<PageEffect> undo(
        Transaction& transaction,
        const UndoDescriptor& undo) override {
        transaction_ids.push_back(transaction.id());
        undos.push_back(undo);
        return {effect(9, 'u')};
    }

    std::vector<TransactionId> transaction_ids;
    std::vector<UndoDescriptor> undos;
};

struct ManagerFixture {
    ManagerFixture()
        : log(config()),
          manager(log, lock_manager, undo_executor) {
        log.open(directory.path.string());
    }

    TempDir directory;
    Log log;
    LockManager lock_manager;
    RecordingUndoExecutor undo_executor;
    TransactionManager manager;
};

TEST(TransactionManagerTest, BeginCreatesOwnedTransactionAndWalRecord) {
    ManagerFixture fixture;

    const TransactionHandle transaction = fixture.manager.begin();

    EXPECT_EQ(transaction->id(), 1u);
    EXPECT_EQ(transaction->state(), TransactionState::Active);
    EXPECT_EQ(transaction->last_lsn(), 1u);
    EXPECT_EQ(fixture.manager.find(transaction->id()), transaction);

    const WalRecord begin = fixture.log.read(transaction->last_lsn());
    EXPECT_EQ(begin.type, WalRecordType::TxnBegin);
    EXPECT_EQ(begin.transaction_id, transaction->id());
    EXPECT_EQ(begin.prev_lsn, 0u);

    EXPECT_EQ(
        fixture.manager.abort(transaction, AbortReason::ClientRequest),
        AbortStatus::Success);
}

TEST(TransactionManagerTest, CommitMakesDecisionDurableAndRemovesTransaction) {
    ManagerFixture fixture;
    const TransactionHandle transaction = fixture.manager.begin();

    EXPECT_EQ(fixture.manager.commit(transaction), CommitStatus::Success);

    EXPECT_EQ(transaction->state(), TransactionState::Committed);
    EXPECT_FALSE(fixture.manager.find(transaction->id()));
    EXPECT_GE(fixture.log.durable_lsn(), transaction->last_lsn());

    const WalRecord commit = fixture.log.read(transaction->last_lsn());
    EXPECT_EQ(commit.type, WalRecordType::TxnCommit);
    EXPECT_EQ(commit.prev_lsn, 1u);
    EXPECT_EQ(
        fixture.manager.commit(transaction),
        CommitStatus::TransactionNotFound);
}

TEST(TransactionManagerTest, AbortWritesDecisionAndDurableEnd) {
    ManagerFixture fixture;
    const TransactionHandle transaction = fixture.manager.begin();

    EXPECT_EQ(
        fixture.manager.abort(transaction, AbortReason::StatementFailure),
        AbortStatus::Success);

    EXPECT_EQ(transaction->state(), TransactionState::Aborted);
    EXPECT_FALSE(fixture.manager.find(transaction->id()));
    EXPECT_GE(fixture.log.durable_lsn(), transaction->last_lsn());

    const std::vector<WalRecord> records = fixture.log.scan();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].type, WalRecordType::TxnBegin);
    EXPECT_EQ(records[1].type, WalRecordType::TxnAbort);
    EXPECT_EQ(records[2].type, WalRecordType::TxnEnd);
    EXPECT_EQ(records[2].prev_lsn, records[1].lsn);
    EXPECT_EQ(
        std::get<AbortPayload>(WalRecords::decode(records[1])).reason,
        AbortReason::StatementFailure);
}

TEST(TransactionManagerTest, AbortUndoesActionsAndChainsCompensationRecord) {
    ManagerFixture fixture;
    const TransactionHandle transaction = fixture.manager.begin();

    PendingBTreeAction action(transaction->id(), transaction->last_lsn());
    action.set_undo(InsertUndo{KeyCodec::make_string("key")});
    action.add_effect(effect(7, 'a'));
    const Lsn action_lsn = fixture.manager.append_action(transaction, action.build());

    EXPECT_EQ(
        fixture.manager.abort(transaction, AbortReason::ClientRequest),
        AbortStatus::Success);

    ASSERT_EQ(fixture.undo_executor.transaction_ids.size(), 1u);
    EXPECT_EQ(fixture.undo_executor.transaction_ids[0], transaction->id());
    EXPECT_TRUE(std::holds_alternative<InsertUndo>(fixture.undo_executor.undos[0]));

    const std::vector<WalRecord> records = fixture.log.scan();
    ASSERT_EQ(records.size(), 5u);
    EXPECT_EQ(records[1].lsn, action_lsn);
    EXPECT_EQ(records[2].type, WalRecordType::TxnAbort);
    EXPECT_EQ(records[3].type, WalRecordType::Compensation);
    EXPECT_EQ(records[4].type, WalRecordType::TxnEnd);

    const auto compensation =
        std::get<CompensationPayload>(WalRecords::decode(records[3]));
    EXPECT_EQ(compensation.undo_of_lsn, action_lsn);
    EXPECT_EQ(compensation.undo_next_lsn, records[0].lsn);
    EXPECT_EQ(records[3].prev_lsn, records[2].lsn);
    EXPECT_EQ(records[4].prev_lsn, records[3].lsn);
}

TEST(TransactionManagerTest, AppendActionValidatesTransactionWalChain) {
    ManagerFixture fixture;
    const TransactionHandle transaction = fixture.manager.begin();

    PendingBTreeAction action(transaction->id(), transaction->last_lsn());
    action.set_undo(InsertUndo{KeyCodec::make_string("key")});
    action.add_effect(effect(7, 'a'));
    PendingWalRecord pending = action.build();
    pending.prev_lsn += 1;

    EXPECT_THROW(
        fixture.manager.append_action(transaction, std::move(pending)),
        std::invalid_argument);
    EXPECT_EQ(transaction->last_lsn(), 1u);
    EXPECT_EQ(fixture.log.next_lsn(), 2u);

    EXPECT_EQ(
        fixture.manager.abort(transaction, AbortReason::ClientRequest),
        AbortStatus::Success);
}

TEST(TransactionManagerTest, WaitRegistrationDetectsCyclesAndMissingTransactions) {
    ManagerFixture fixture;
    const TransactionHandle first = fixture.manager.begin();
    const TransactionHandle second = fixture.manager.begin();

    const std::vector<TransactionId> second_blocker = {second->id()};
    EXPECT_EQ(
        fixture.manager.register_wait(first->id(), second_blocker),
        WaitRegistrationStatus::Registered);

    const std::vector<TransactionId> first_blocker = {first->id()};
    EXPECT_EQ(
        fixture.manager.register_wait(second->id(), first_blocker),
        WaitRegistrationStatus::Deadlock);

    const std::vector<TransactionId> missing_blocker = {99};
    EXPECT_EQ(
        fixture.manager.register_wait(first->id(), missing_blocker),
        WaitRegistrationStatus::BlockerNotFound);
    EXPECT_EQ(
        fixture.manager.register_wait(99, first_blocker),
        WaitRegistrationStatus::TransactionNotFound);

    fixture.manager.remove_wait(first->id());
    EXPECT_EQ(
        fixture.manager.register_wait(second->id(), first_blocker),
        WaitRegistrationStatus::Registered);

    EXPECT_EQ(
        fixture.manager.abort(second, AbortReason::ClientRequest),
        AbortStatus::Success);
    EXPECT_EQ(
        fixture.manager.abort(first, AbortReason::ClientRequest),
        AbortStatus::Success);
}

TEST(TransactionManagerTest, RejectsHandleOwnedByAnotherManager) {
    ManagerFixture first;
    ManagerFixture second;
    const TransactionHandle foreign = first.manager.begin();

    EXPECT_EQ(
        second.manager.commit(foreign),
        CommitStatus::TransactionNotFound);
    EXPECT_EQ(
        first.manager.abort(foreign, AbortReason::ClientRequest),
        AbortStatus::Success);
}

TEST(TransactionManagerTest, ReopenContinuesAfterGreatestTransactionId) {
    TempDir directory;
    LockManager first_lock_manager;
    RecordingUndoExecutor first_undo_executor;

    {
        Log log(config());
        log.open(directory.path.string());
        TransactionManager manager(log, first_lock_manager, first_undo_executor);
        const TransactionHandle transaction = manager.begin();
        EXPECT_EQ(transaction->id(), 1u);
        EXPECT_EQ(manager.commit(transaction), CommitStatus::Success);
        log.close();
    }

    LockManager second_lock_manager;
    RecordingUndoExecutor second_undo_executor;
    Log reopened(config());
    reopened.open(directory.path.string());
    TransactionManager manager(reopened, second_lock_manager, second_undo_executor);

    const TransactionHandle transaction = manager.begin();
    EXPECT_EQ(transaction->id(), 2u);
    EXPECT_EQ(
        manager.abort(transaction, AbortReason::ClientRequest),
        AbortStatus::Success);
}

} // namespace
