#pragma once

#include <Log/WalPayload.h>

#include <cstdint>
#include <optional>
#include <vector>

class PendingBTreeAction {
public:
    PendingBTreeAction(std::uint64_t transaction_id, Lsn prev_lsn);

    void set_undo(UndoDescriptor undo);
    void add_effect(PageEffect effect);
    PendingWalRecord build() const;

private:
    std::uint64_t transaction_id_;
    Lsn prev_lsn_;
    std::optional<UndoDescriptor> undo_;
    std::vector<PageEffect> effects_;
};
