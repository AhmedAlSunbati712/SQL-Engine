# Client + Session structure in a server-client architecture
At startup, the user initializes a client that it uses to establish connections/sessions with the database. Any operation to act on the database should be done through the sessions. The strucutre of the client should be roughly as the following:
```
Client:
    Sessions[], A list of currently active sessions, each with its own tcp connection to the server
    db_addr

    get_session()
    close_session(Session *)
```
When the user creates a new session, the client will establish the tcp connection with the database and pass the socket file descriptor to the session object. The session object will then be responsible for the lifetime of this descriptor.

Session is closer to the key-store layer. It exposes key-store semantics over tcp to the client. It handles internally the process of converting user requests into packets to be sent over the tcp connection. It basically exposes the same interface as KeyStore.h but we will limit it now to a couple of operations for testing purposes

```
Session:
    socket_fd
    get(key)
    put(key, value)
    delete(key)
    begin_transaction()
    commit()
    rollback()
```

Each message sent from the session to the server should identify it's operator and the operands. There are three different types of operators:
- nullary operators: They don't have operands. Examples: begin_transaction, commit, rollback
- unary operators: They have one operand. Examples: get, delete
- binary operators: they have two operands. Examples: put

so we would probably need to have the following structures:
```
1 byte
enum OperatorType {
    Nullary
    Unary
    Binary
}

1 byte
enum Operator {
    GET
    PUT
    DELETE
    BEGIN_TXN
    COMMIT
    ROLLBACK
}

1 byte
struct Command {
    OperatorType
    Operator
    std::optional Key = std::nullopt
    std::optional Value = std::nullopt
}
```
A couple of things to handle:
Type conversions. Currently the KeyStore class expects std::variant for both the key and the value. This adds a bit complexity since the client now will have to detect the different primitive types, then encode those primitive types to send over wire and then the server will have to detect what primitive types it got and then internally conver it to the Key and Value structs. Too much complexity. Make the KeyStore.h only accespt Key and Value parameters. no std::variant. the client will accept the vairant and internally convert those into key/value structs to be serialized with the command struct.

As usual, we will follow the following layout for encoding commands:
- PayloadSize (4 bytes)
- OperatorType (1 byte)
- Operator (1 byte)
- Key size if it exists (4 bytes). Otherwise this is not in the packet
- Key if it exists. Otherwise, this is not in the packet
- Value size if it exists (4 bytes). Otherwise this is not in the packet
- Value

Let's have a NetCodec that can serialize command structs into a vector of bytes (std::uint8_t) and deserialize back into structs.
We also need to refactor encoding/decoding of keys/values out of the keystore. it should be it's own indpendend module
KeyCodec
ValueCodec

I know we already have those but we need to move the encode/decode functions there to it