# DESIGNDOC: SyncText Lock-Free CRDT System

## 1. System Architecture

### High-Level Overview

SyncText operates as a distributed, lock-free, multi-process collaborative editor. Each user runs an independent editor instance, but all instances stay synchronized through a decentralized messaging and merge pipeline. The system avoids centralized servers and avoids locks, so each instance works autonomously and efficiently.

Core workflow:
1. **Local file changes** are detected through a file monitor.
2. These changes are converted into **CRDT update objects**.
3. Updates are broadcast to all users using **POSIX Message Queues**.
4. Each instance receives incoming updates through a **listener thread**.
5. Updates are merged into the local CRDT document via **LWW conflict resolution**.
6. The merged CRDT state is written back to the user's local file.

Key architectural characteristics:
- **Fully distributed**: No master node; every user is equal.
- **Event-driven sync**: Each detected edit becomes an event propagated to all peers.
- **Lock-free design**: All shared structures use atomics; no blocking between threads.
- **Deterministic merging**: LWW ensures all users converge to the same final document.
- **Self-healing**: If a user reconnects or restarts, the queue + CRDT ensures state convergence.

This architecture allows real-time multi-user editing with minimal overhead, high throughput, and predictable merge behavior.

---

#### How the System Operates (Algorithmic View)

**1. File Monitoring Algorithm**
```
INPUT: last_snapshot, current_file
LOOP every 50–100 ms:
    read current_file → cur
    IF cur unchanged:
        CONTINUE
    FOR i from 0 to max(len(last_snapshot), len(cur)):
        a = last_snapshot[i] or EMPTY
        b = cur[i] or EMPTY
        IF a != b:
            create UPDATE(pos=i, char=b, ts=now, user=my_id)
            enqueue UPDATE to message queue
    last_snapshot = cur
```

**2. Broadcast Algorithm**
```
WHILE updates_pending:
    IF batch_size < threshold:
        accumulate updates
    ELSE:
        serialize batch
        send batch to global MQ
```

**3. Listener + Merge Algorithm (LWW CRDT)**
```
LOOP receiving UPDATE u from MQ:
    IF u.pos >= document_length:
        extend CRDT document

    cell = CRDT[u.pos]

    IF (u.timestamp > cell.timestamp) OR
       (u.timestamp == cell.timestamp AND u.user_id > cell.user_id):
           APPLY u.char to cell
           UPDATE cell.timestamp = u.timestamp
           UPDATE cell.user_id = u.user_id

    WRITE materialized document to file
```

**4. Materialization Algorithm**
```
OUTPUT = empty string
FOR each cell in CRDT:
    IF cell.char != EMPTY:
        append cell.char to OUTPUT
RETURN OUTPUT
```

**5. Example Flow (Algorithmic)**
```
Initial document: "Hello"
User1 inserts '!': generate UPDATE(pos=5, char='!', ts=101)
User2 replaces 'e'→'a': generate UPDATE(pos=1, char='a', ts=102)

Both users receive both updates.
LWW picks highest timestamp entry at every position.

Final CRDT state → "Hallo!"
```

---

### Major Components

1. **Registry Manager (Shared Memory)**

   - Stores list of active users and file metadata
   - Uses atomic operations for lock-free updates

2. **File Monitor**

   - Watches the local text file for modifications
   - Produces update objects when content changes

3. **Message Queue Broadcaster**

   - Sends update objects to all other users
   - Format: serialized struct containing position, char, timestamp, userID

4. **Listener Thread**

   - Continuously reads update objects from the queue
   - Triggers merge routine

5. **CRDT Merge Engine**

   - Applies LWW (Last Writer Wins) logic
   - Maintains vector of characters with metadata

### Key Data Structures

```
struct Update {
    int pos;
    char ch;
    long long ts;   // timestamp
    int user_id;
};

struct Entry {
    char ch;
    long long ts;
    int user_id;
};
```

- Global CRDT document: `vector<Entry>`
- Message queue buffer: fixed-size array of serialized updates
- Shared-memory registry: atomic user list

---

## 2. Implementation Details

### Change Detection (File Monitoring)

- Uses stat() polling every 50–100ms
- If file size or mtime changed:
  - Diff with previous snapshot
  - Generate updates for inserted/updated/deleted characters
- Example:
  - Before: `abc`
  - After: `abXc`
  - Detected: insert 'X' at pos=2

### Message Queues & Shared Memory Structure

- POSIX Message Queues (`mq_open`, `mq_send`, `mq_receive`)
- Single global queue: each update broadcast once
- Shared memory (`shm_open`) stores:
  - Active users
  - Their assigned queue names

### CRDT Merge Algorithm (LWW)

- On receiving external update:
  - If incoming timestamp > local timestamp → overwrite
  - Else ignore
- Deletes encoded as special char `\0`
- Ensures deterministic order

**Example:** Local: pos 5='A' ts=100 Remote: pos 5='B' ts=120 → overwrite

### Thread Architecture

1. **Main Thread**

   - Initializes shared memory, registry, and file state

2. **Monitor Thread**

   - Detects local edits and pushes updates to queue

3. **Broadcast Thread**

   - Serializes update objects and sends to MQ

4. **Listener Thread**

   - Receives remote updates
   - Triggers CRDT merge
   - Writes merged result to file

Thread communication uses lock-free atomic queues and CAS operations.

---

## 3. Design Decisions

### Lock-Free Operation

- No mutexes for editor-critical paths
- Used atomics for:
  - Timestamps
  - Registry updates
  - CRDT entry writes
- Message queue provides system-level synchronization without blocking

### Rationale for LWW

- Simple to implement
- Works well with single-character granularity
- Deterministic merges under concurrency

### cell-wise merge

index: 0 1 2 3 4 5 6 7 8 9 ...<br>
cell : c c c c c c c c c c ...

ex:-
col0: U1=' '  U2='5'  → '5' <br>
col1: U1=' '  U2='6'  → '6' <br>
col2: U1=' '  U2='7'  → '7' <br>
col3: U1=' '  U2=' '  → ' ' <br>
col4: U1=' '  U2=' '  → ' ' <br>
col5: U1='1' U2=' '  → '1' <br>
col6: U1='2' U2=' '  → '2' <br>
col7: U1='3' U2=' '  → '3' <br>
col8: U1='4' U2=' '  → '4' <br>

final o/p : "567  1234"



### Trade-offs

- LWW sacrifices intention preservation
- Char-level CRDT leads to more events but simpler logic
- Polling-based file monitoring increases CPU use slightly

---

## 4. Challenges & Solutions

### 1. Partial Character Overwrite

**Issue:** Updating a multi-digit number replaced only the first digit. **Fix:** Treat each changed region as a slice update, not char-by-char when detecting multi-character edits.

### 2. Delete Propagation Errors

**Issue:** Remote delete wasn't removing chars locally. **Fix:** Encode delete as `{'\0', ts, user}` and clean during merge.

### 3. MQ Throttling & Batching

**Issue:** Too many messages under heavy typing. **Fix:** Added batching threshold (5 updates) before broadcast.

### 4. CRDT Divergence

**Issue:** Merge order caused mismatched final text. **Fix:** LWW rule normalized: `if (incoming.ts > local.ts) apply()`.

### 5. File Rewrite Race

**Issue:** Listener and monitor both writing file simultaneously. **Fix:** Single-writer design: only listener writes final merged file.

---
***Example of correctness***

user_1 write : 1 2 3 (u1 3 update)<br>
user_2 write : 3 4 5 (u1 3 update)<br>
user_1 update : 2 -> 7 (u1 4 update)<br>
user_2 write 7 8 (u2 5 update) <br>
user_2_doc :: 3 4 5 7 8<br>
broadcast and marge <br>
user_1_doc :3 7 5 7 8 ( u1 4 update)<br>
user_1 update : 5 -> 100 ( u1 5 update)<br>
broadcast and marge<br>
noe both doc will : 3 7 100 7 8<br>