# random notes not related to server
transactions should keep track of:
- LSNs associated with them
- Locks acquired
- latches acquired. Actually that's the pager's job
# Server View
A main while loop that listens for incoming connection requests. It then accepts these connections, and dispatches a thread that listens on this socket trying to listen for commands, dispatches a thread to execute the command, get the result and write back to the user. the Executor thread is not gonna be detached. it's going to be joined. The questions is how do we grab the result from the dispatched thread? we can construct an instance of a KeyStoreStatus, KeyStoreGetResult, or KeyStoreRemoveResult depending on the operation and pass it by reference to the thread and it will hold the result in there. We will need handlers for each operation. No matter which operation any handler is dealing with, all of them will at least have a reference to a KeyStore instance + a reference to the result object they are going to store their result in. Finally each handler will get their own args too depending on the operation.
# Algorithmic Flows
Main server thread:
```
Start up the server and listen in 127.0.0.1:8080
Stand up a keystore instance successfully and connect to the DB
while True:
    accept connections
    start an command dispatcher thread, pass in the socket connection, a reference to the keystore, and detach the thread
```
We are going to keep it simple for now. But later we will add:
- Max concurrent connections
- Handlind idle clients
- Cleaning up connections on exit and rolling back if needed

command dispatcher thread flow:
```
while True:
    read 4 bytes to get the payload size
    then read the full payload (command)
    switch (command):
        for each command type, dispatch an executor thread that takes in: KeyStore, struct to store the result of the command, the comand args if it takes arguments
        join on that thread
    send the result back to the client
```

command executors are just handler functions. Each command gets its own handler:
handle_get(keystore reference, reference to struct to store result, args...)
handle_remove
handle_commit...etc


The command dispatcher should also keep track if there's a current active transaction or not (set by the BEGIN_TXN command and unset by COMMIT or rollback) to know whether to rollback or not when the client closes off their connection or crash.

The question is whether we should model the server as a class and have the program entrypoint in a different server file that has a main or not. i dont thinnk we need the class currently. So we probably should go with the simple loops.