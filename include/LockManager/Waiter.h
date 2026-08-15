#pragma once
#include <Transaction.h>
#include <LockMode.h>

struct Waiter {
    TransactionId txn_id;
    LockMode lock_mode;
    bool granted = false;
};