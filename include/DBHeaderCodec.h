#pragma once
#include <Page.h>
#include <span>
#include <cstdint>


namespace DBHeaderCodec {
    void serialize_DBHeader(DBHeader &db_header, char *out);
    void deserialize_DBHeader(DBHeader &db_header, char *in);
    bool validate_DBHeader(DBHeader &db_header);
}