#pragma once
#include <Journal.h>
#include <span>
#include <cstdint>


namespace Journal {
    // Encoding Utils
    void serialize_jHeader(JournalHeader &jHeader, char *out);
    void deserialize_jHeader(JournalHeader &jHeader, char *in);
    void serialize_jPage_record(JournalPageRecord &jPage_record, char *out);
    void deserialize_jPage_record(JournalPageRecord &jPage_record, char *in);

    std::uint32_t generate_nonce();
	std::uint32_t checksum(uint32_t nonce, std::span<const char> data);
	bool validate_journal_record_checksum(JournalPageRecord &jPage_record, JournalHeader& jHeader);
}