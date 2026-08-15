#include <gtest/gtest.h>

#include <TransactionManager/Transaction.h>

#include <type_traits>

TEST(TransactionTest, StartsActiveWithRequestedIdentity) {
    Transaction transaction(42);

    EXPECT_EQ(transaction.id(), 42u);
    EXPECT_EQ(transaction.state(), TransactionState::Active);
}

TEST(TransactionTest, RejectsReservedZeroIdentity) {
    EXPECT_THROW(Transaction(0), std::invalid_argument);
}

TEST(TransactionTest, TransactionIdentityCannotBeCopiedOrMoved) {
    static_assert(!std::is_copy_constructible_v<Transaction>);
    static_assert(!std::is_copy_assignable_v<Transaction>);
    static_assert(!std::is_move_constructible_v<Transaction>);
    static_assert(!std::is_move_assignable_v<Transaction>);
}
