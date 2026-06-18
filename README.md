# os-library

A distributed library system built for the 2025–2026 Operating Systems course.

Multiple **library processes** (written in C) each own a catalog of books and cooperate over
**named pipes (FIFOs)** to satisfy user requests. A book exists exactly once in the whole
system: if it is borrowed from any library it is unavailable everywhere until returned. Users
interact with the system through Bash scripts.

---

## Requirements

- Linux (developed/tested on Ubuntu 24.04)
- `gcc` with POSIX threads
- `bash`, `make`

> The system uses POSIX FIFOs under `/tmp`, POSIX threads, and POSIX signals, so it must run on
> a Unix-like OS. It does **not** run natively on Windows.

---

## Build & run

```sh
make build                              # compiles ./library, marks the scripts executable
make run ARGS="3 csv_books/books.csv"   # bootstraps a scenario (3 libraries from books.csv)
make clean                              # removes the binary and stale IPC (/tmp/lib_cmd*, /tmp/catalog*.csv)
```

`make run` forwards `ARGS` to `bootstrap.sh`.

### Quick start

```sh
make build
./bootstrap.sh 3 csv_books/books.csv

./user.sh Alice   1 register
./user.sh Alice   1 borrow "The Great Gatsby"
./user.sh Alice   1 return "The Great Gatsby"
./user.sh Charlie 1 search --by author "Isaac Asimov"

./manage.sh list_books
./manage.sh list_users
./manage.sh status
./manage.sh stop
```

---

## Components

| File                  | Role                                                                       |
|-----------------------|----------------------------------------------------------------------------|
| `library.c`           | The library process: catalog, user/inter-library request handling, IPC.    |
| `library_types.h`     | Shared data structures (`Book`, `User`, `PendingRequest`, contexts, …).     |
| `bootstrap.sh`        | Splits the source CSV into N catalogs and launches N libraries.             |
| `user.sh`             | User-facing client: register / search / borrow / return.                   |
| `manage.sh`           | Admin script: status / list_books / list_users / stop.                     |
| `Makefile`            | `build` / `clean` / `run` targets.                                         |
| `csv_books/books.csv` | Source catalog (`Title,Author,Year`, with a header row).                   |

---

## Interfaces

### Bootstrapping
```sh
./bootstrap.sh <num_libraries> <source_csv>
```
Strips the CSV header, round-robins the rows into `/tmp/catalog<i>.csv`, then launches one
library per catalog and waits until each library's command FIFO is ready.

### Library process
```sh
./library <library_id> <num_total_libraries> <catalog_file>
```
Normally launched by `bootstrap.sh`, not by hand. Library IDs are assumed to be contiguous
`1..N`.

### User script
```sh
./user.sh <username> <library_id> <operation> [args]

./user.sh Alice   1 register
./user.sh Alice   1 borrow "The Great Gatsby"
./user.sh Bob     2 return "The Great Gatsby"
./user.sh Charlie 1 search --by author "Isaac Asimov"
./user.sh Charlie 1 search --by title  "1984"
./user.sh Charlie 1 search --by year   1984
```
Usernames are normalized to uppercase. A user must `register` with a library before borrowing,
may register with as many libraries as they want, and may hold at most one book at a time.

### Management script
```sh
./manage.sh status        # SIGUSR1 -> each library dumps library_status_<id>.txt; printed back
./manage.sh list_books    # lists every book in every library and its availability
./manage.sh list_users    # lists registered users per library and the book each holds
./manage.sh stop          # SIGTERM to all libraries, then cleans up IPC resources
```

---

## Architecture

### Processes & threads
Each library is a single process with:

- A **listener thread** reading the library's command FIFO (`/tmp/lib_cmd_<id>`), opened
  `O_RDWR` so it never sees EOF when clients disconnect. It splits the byte stream into
  newline-delimited messages and dispatches each.
- A **detached worker thread per request** — user operations, inbound inter-library requests
  (`BORROW` / `VERIFY` / `SEARCH`), and management queries are each handled on their own thread,
  so requests run **concurrently**.
- The **main thread**, which after setup blocks in `sigwait()` and handles `SIGUSR1` (status
  dump) and `SIGTERM` (clean shutdown) synchronously — making it safe to do file I/O and free
  resources without racing the workers. `SIGUSR1`/`SIGTERM` are blocked process-wide before any
  thread is spawned, and `SIGPIPE` is ignored so a client closing its pipe early can't crash us.

### IPC
- **FIFOs** are the only IPC channel. Every library has one well-known command FIFO,
  `/tmp/lib_cmd_<id>`, carrying user, inter-library, and management messages — all short and
  newline-framed.
- Clients (and, for search, peers) write bulk replies to a **separate response FIFO**, never the
  command FIFO, so large payloads can't corrupt the framed command channel.
- Inter-library writes use `O_WRONLY | O_NONBLOCK` so a library never blocks (and never deadlocks
  against its own listener) when contacting a busy or dead peer.

### Concurrency & synchronization
- One mutex **per book** and one **per user** guard the mutable state; two users borrowing
  different books never contend. Locks are always taken **user-before-book** to avoid deadlock.
- A global `users_lock` guards the user registry (including growth via `realloc`).
- `PendingRequest` slots (a fixed pool) coordinate inter-library request/response: an atomic
  `in_use` flag plus a `request_id` check cause late or stale responses to be ignored, and the
  slot's condition variable + `received_responses` counter implement a quorum/TTL wait.
- As required by the spec, each **user** request incurs one random **1–5 s** processing delay
  (applied once on the user-facing path). Inter-library messages are left undelayed so nested
  cross-library waits don't compound.

### Borrowing across libraries
1. A user asks library *A* to borrow a book.
2. If *A* owns the book, it lends locally under the per-book lock.
3. If not, *A* broadcasts `LIB|REQUEST|…|BORROW|<title>|<user>` to all peers and waits (with a
   **TTL**, `BROADCAST_TIMEOUT_SEC`) on a `PendingRequest` slot.
4. Peers reply `GRANTED` / `ALREADY_LENT` / `NOT_FOUND`. Because each book exists once, at most
   one peer can grant it; the owner records `lent_to_lib` (the borrowing library) and the
   borrowing user's name.
5. The user always **returns to the library they borrowed from**; that library forwards a
   `LIB|RETURN` to the owner, which marks the book available again.

### Optimistic lending & the VERIFY reconciliation
A book lent to a peer is marked `LENT_OUT` but flagged **unconfirmed** (`really_lent = 0`): the
owner can't be sure the borrower received the grant (the Two Generals problem). Rather than block
or run a commit protocol, the doubt is resolved **lazily**:

- When anyone next wants that book (a local user, *or* another library), the owner sends
  `LIB|REQUEST|…|VERIFY|<title>|<user>` to the borrowing library.
- The borrower answers `HELD` (its user still holds it → the owner caches `really_lent = 1` and
  refuses) or `NOT_HELD` (the grant was lost → the owner **reclaims** the book and lends it).

This is paid only when a book is actually contended, and never causes a double-lend.

### Distributed search
Search results can exceed a single pipe read, so they must not travel over the command FIFO. The
contacted library writes its own matches to the user's response pipe, then broadcasts
`LIB|REQUEST|…|SEARCH|<field>|<value>|<resp_pipe>`. Each peer opens that pipe directly, streams
its matching books, and sends a small `DONE` ack over the command FIFO. The contacted library
blocks on the pending-slot quorum until every peer has acked (or the TTL fires), then closes the
pipe — so the user receives one combined result set. Only the tiny query and acks ride the
command FIFO; the bulk data goes peer-to-user directly.

---

## Wire protocol (FIFO messages)

`|`-delimited records, one per line.

```
# User -> library
USER|REGISTER|<user>|<resp_fifo>
USER|BORROW|<user>|<title>|<resp_fifo>
USER|RETURN|<user>|<title>|<resp_fifo>
USER|SEARCH|<user>|<field>|<value>|<resp_fifo>          # field = author | title | year

# Library -> library   (REQUEST is the envelope; the 5th field is the operation)
LIB|REQUEST|<src>|<id>|BORROW|<title>|<user>
LIB|REQUEST|<src>|<id>|VERIFY|<title>|<user>
LIB|REQUEST|<src>|<id>|SEARCH|<field>|<value>|<resp_fifo>
LIB|RESPONSE|<responder>|<id>|<GRANTED|ALREADY_LENT|NOT_FOUND|HELD|NOT_HELD|DONE>
LIB|RETURN|<src>|<title>

# Management -> library
MGMT|LIST_BOOKS|<resp_fifo>
MGMT|LIST_USERS|<resp_fifo>
```

Replies to user requests are `<code>|<message>` (the status line), optionally followed by extra
lines (e.g. search results).

---

## Error / status codes

The library answers every user request with a leading numeric code; `user.sh` exits `0` on code
`0` and `1` otherwise.

| Code | Meaning                                                              |
|------|---------------------------------------------------------------------|
| 0    | Success (registered / lent / returned / search results)             |
| 1    | No such user, or book found in no catalog at all                    |
| 2    | User already registered, or invalid search field                    |
| 3    | No such book (on return)                                            |
| 4    | Book is already lent out                                            |
| 5    | User has no book to return                                          |
| 6    | System busy / allocation failure — retry                            |
| 7    | User already holds a book (must return before borrowing another)    |
| 8    | User does not hold that particular book                             |

---

## Cleanup

`make clean` removes the binary and stale IPC (`/tmp/lib_cmd*`, `/tmp/catalog*.csv`).
`./manage.sh stop` sends `SIGTERM` to every library (each frees its own catalog, user registry,
and mutexes, and unlinks its command FIFO) and then clears any leftover IPC. The two are
one-directional — `manage.sh stop` may call `make clean`, but `clean` never calls back — so they
cannot recurse.
