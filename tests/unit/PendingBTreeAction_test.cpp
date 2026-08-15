#include <gtest/gtest.h>

#include <KeyCodec.h>
#include <Log/PendingBTreeAction.h>
#include <Log/WalRecords.h>
#include <V2PageCodec.h>

namespace {
PageEffect effect(PageEffectKind kind, std::uint32_t page_num, char marker) {
    PageEffect result{.kind = kind, .page_num = page_num};
    V2PageCodec::initialize(result.after_image, page_num, V2PageKind::BTreeLeaf);
    result.after_image[V2_PAGE_HEADER_SIZE] = marker;
    return result;
}

TEST(PendingBTreeActionTest, RequiresUndoAndEffects) {
    PendingBTreeAction action(1, 1);
    EXPECT_THROW(action.build(), std::logic_error);
    action.set_undo(InsertUndo{KeyCodec::make_string("k")});
    EXPECT_THROW(action.build(), std::logic_error);
    EXPECT_THROW(action.set_undo(InsertUndo{KeyCodec::make_string("k")}), std::logic_error);
}

TEST(PendingBTreeActionTest, DeduplicatesEffectsAndPreservesAllocation) {
    PendingBTreeAction action(1, 1);
    action.set_undo(InsertUndo{KeyCodec::make_string("k")});
    action.add_effect(effect(PageEffectKind::Allocate, 7, 'a'));
    action.add_effect(effect(PageEffectKind::Write, 7, 'b'));
    auto payload = std::get<BTreeActionPayload>(WalRecords::decode(WalRecord{
        .lsn = 2, .type = action.build().type, .transaction_id = 1,
        .prev_lsn = 1, .data = action.build().data}));
    ASSERT_EQ(payload.effects.size(), 1u);
    EXPECT_EQ(payload.effects[0].kind, PageEffectKind::Allocate);
    EXPECT_EQ(payload.effects[0].after_image[V2_PAGE_HEADER_SIZE], 'b');
}

TEST(PendingBTreeActionTest, FreeReplacesPreviousEffect) {
    PendingBTreeAction action(1, 1);
    action.set_undo(InsertUndo{KeyCodec::make_string("k")});
    action.add_effect(effect(PageEffectKind::Write, 7, 'a'));
    action.add_effect(effect(PageEffectKind::Free, 7, 'b'));
    auto pending = action.build();
    auto payload = std::get<BTreeActionPayload>(WalRecords::decode({.lsn = 2, .type = pending.type,
        .transaction_id = 1, .prev_lsn = 1, .data = pending.data}));
    ASSERT_EQ(payload.effects.size(), 1u);
    EXPECT_EQ(payload.effects[0].kind, PageEffectKind::Free);
}
} // namespace
