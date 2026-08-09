#include <ValueCodec.h>

#include <limits>
#include <type_traits>

namespace valuecodec {

namespace {

std::vector<char> encode_varuint_bytes(std::uint64_t value) {
    // VarUInt uses 7 payload bits per byte.
    // The high bit says whether another byte follows.
    std::vector<char> out;

    do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7Fu);
        value >>= 7;
        if (value != 0) byte |= 0x80u;
        out.push_back(static_cast<char>(byte));
    } while (value != 0);

    return out;
}

bool decode_varuint_bytes(const std::vector<char> &bytes, std::uint64_t *out) {
    if (bytes.empty() || out == nullptr) return false;

    // Reconstruct the integer 7 bits at a time.
    // We also reject malformed encodings that never terminate
    // or try to spill past 64 bits.
    std::uint64_t value = 0;
    std::uint32_t shift = 0;

    for (std::size_t i = 0; i < bytes.size(); i++) {
        std::uint8_t byte = static_cast<std::uint8_t>(bytes[i]);

        if (shift >= 64 && (byte & 0x7Fu) != 0) return false;
        value |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;

        if ((byte & 0x80u) == 0) {
            if (i + 1 != bytes.size()) return false;
            *out = value;
            return true;
        }

        shift += 7;
        if (shift > 63 && i + 1 < bytes.size()) return false;
    }

    return false;
}

std::uint64_t zigzag_encode(std::int64_t value) {
    // ZigZag maps signed integers to unsigned integers in a way
    // that keeps small-magnitude negatives small after encoding.
    return (static_cast<std::uint64_t>(value) << 1) ^ static_cast<std::uint64_t>(value >> 63);
}

std::int64_t zigzag_decode(std::uint64_t value) {
    return static_cast<std::int64_t>((value >> 1) ^ (~(value & 1) + 1));
}

} // namespace

std::optional<Value> encode(const ValueInput &input) {
    return std::visit(
        [](const auto &value) -> std::optional<Value> {
            using T = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<T, bool>) {
                return make_bool(value);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                return make_varuint(value);
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return make_varint(value);
            } else {
                if (value.size() > MAX_PAYLOAD_SIZE) return std::nullopt;
                return make_char(value);
            }
        },
        input
    );
}

std::optional<ValueInput> decode(const Value &value) {
    if (!validate_value(value)) return std::nullopt;

    switch (value.type) {
        case ValueType::VarUInt: {
            std::uint64_t decoded = 0;
            if (!decode_varuint(value, &decoded)) return std::nullopt;
            return ValueInput{decoded};
        }
        case ValueType::VarInt: {
            std::int64_t decoded = 0;
            if (!decode_varint(value, &decoded)) return std::nullopt;
            return ValueInput{decoded};
        }
        case ValueType::Bool:
            return ValueInput{value.data[0] == '\1'};
        case ValueType::Char:
            return ValueInput{std::string(value.data.begin(), value.data.end())};
    }

    return std::nullopt;
}

bool equal(const Value &lhs, const Value &rhs) {
    return lhs.type == rhs.type && lhs.size == rhs.size && lhs.data == rhs.data;
}

bool validate_value(const Value &value) {
    if (value.size != value.data.size()) return false;
    if (value.size > MAX_PAYLOAD_SIZE) return false;

    switch (value.type) {
        case ValueType::VarUInt: {
            // A VarUInt value is valid only if the byte sequence is a well-formed
            // unsigned varint that can be fully decoded.
            std::uint64_t decoded = 0;
            return decode_varuint_bytes(value.data, &decoded);
        }
        case ValueType::VarInt:
        {
            // The on-disk bytes for VarInt are still an unsigned varint.
            // The signed interpretation happens only after ZigZag decode.
            std::uint64_t decoded = 0;
            return decode_varuint_bytes(value.data, &decoded);
        }
        case ValueType::Bool:
            // Bool is fixed-width in this model: exactly one payload byte.
            return value.size == 1 &&
                (value.data[0] == '\0' || value.data[0] == '\1');
        case ValueType::Char:
            // Char values are just arbitrary raw bytes for now.
            return true;
    }

    return false;
}

Value make_varuint(std::uint64_t value) {
    Value out{};
    out.type = ValueType::VarUInt;
    out.data = encode_varuint_bytes(value);
    out.size = static_cast<std::uint32_t>(out.data.size());
    return out;
}

Value make_varint(std::int64_t value) {
    Value out{};
    out.type = ValueType::VarInt;
    out.data = encode_varuint_bytes(zigzag_encode(value));
    out.size = static_cast<std::uint32_t>(out.data.size());
    return out;
}

Value make_bool(bool value) {
    Value out{};
    out.type = ValueType::Bool;
    out.size = 1;
    out.data.push_back(value ? '\1' : '\0');
    return out;
}

Value make_char(std::string_view value) {
    Value out{};
    out.type = ValueType::Char;
    out.size = static_cast<std::uint32_t>(value.size());
    out.data.assign(value.begin(), value.end());
    return out;
}

bool decode_varuint(const Value &value, std::uint64_t *out) {
    if (value.type != ValueType::VarUInt) return false;
    return decode_varuint_bytes(value.data, out);
}

bool decode_varint(const Value &value, std::int64_t *out) {
    if (value.type != ValueType::VarInt || out == nullptr) return false;

    // First decode the raw unsigned varint bytes, then invert the ZigZag
    // transform to recover the signed integer the caller originally gave us.
    std::uint64_t encoded = 0;
    if (!decode_varuint_bytes(value.data, &encoded)) return false;

    *out = zigzag_decode(encoded);
    return true;
}

} // namespace valuecodec
