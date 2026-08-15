#include <gtest/gtest.h>

#include <TransactionManager/TransactionManager.h>

#include <type_traits>

TEST(TransactionManagerHeaderTest, ManagerOwnershipCannotBeCopiedOrMoved) {
    static_assert(!std::is_copy_constructible_v<TransactionManager>);
    static_assert(!std::is_copy_assignable_v<TransactionManager>);
    static_assert(!std::is_move_constructible_v<TransactionManager>);
    static_assert(!std::is_move_assignable_v<TransactionManager>);
}

TEST(TransactionManagerHeaderTest, ResultTypesHaveDistinctSuccessValues) {
    EXPECT_EQ(CommitStatus::Success, static_cast<CommitStatus>(0));
    EXPECT_EQ(AbortStatus::Success, static_cast<AbortStatus>(0));
    EXPECT_EQ(
        WaitRegistrationStatus::Registered,
        static_cast<WaitRegistrationStatus>(0));
}
