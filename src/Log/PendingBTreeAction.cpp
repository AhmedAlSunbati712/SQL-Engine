#include <Log/PendingBTreeAction.h>

#include <Log/WalRecords.h>

#include <algorithm>
#include <stdexcept>

PendingBTreeAction::PendingBTreeAction(std::uint64_t transaction_id, Lsn prev_lsn)
    : transaction_id_(transaction_id), prev_lsn_(prev_lsn) {
    if (transaction_id_ == 0 || prev_lsn_ == 0) {
        throw std::invalid_argument("Pending B-tree action requires transaction ID and prevLSN");
    }
}

void PendingBTreeAction::set_undo(UndoDescriptor undo) {
    // Logical undo describes the original user operation and must not change
    // as lower storage layers add physical effects.
    if (undo_.has_value()) throw std::logic_error("Logical undo is already set");
    undo_ = std::move(undo);
}

void PendingBTreeAction::add_effect(PageEffect effect) {
    auto existing = std::find_if(effects_.begin(), effects_.end(), [&](const PageEffect& current) {
        return current.page_num == effect.page_num;
    });
    if (existing == effects_.end()) {
        effects_.push_back(std::move(effect));
        return;
    }

    // Allocation remains the page's origin even after later writes replace
    // its image. Free is terminal for the operation and replaces either kind.
    if (existing->kind == PageEffectKind::Allocate && effect.kind == PageEffectKind::Write) {
        effect.kind = PageEffectKind::Allocate;
    }
    *existing = std::move(effect);
}

PendingWalRecord PendingBTreeAction::build() const {
    if (!undo_.has_value()) throw std::logic_error("Pending B-tree action has no logical undo");
    if (effects_.empty()) throw std::logic_error("Pending B-tree action has no page effects");
    return WalRecords::btree_action(transaction_id_, prev_lsn_, {*undo_, effects_});
}
