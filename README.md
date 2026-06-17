# os-library

A distributed library system built for the 2025–2026 Operating Systems course.

Multiple **library processes** (written in C) each own a catalog of books and cooperate over
**named pipes (FIFOs)** to satisfy user requests. A book exists exactly once in the whole
system: if it is borrowed from any library it is unavailable everywhere until returned. Users
interact with the system through Bash scripts.

---

## Requirements

- Linux (developed/tested on Ubuntu 24.04)
- `gcc` with POSIX threads (`-pthread` is pulled in by the standard library on Linux)
- `bash`, `make`

> The system uses POSIX FIFOs under `/tmp` and POSIX signals, so it must run on a
> Unix-like OS. It does **not** run natively on Windows.

---

## Build

```sh
make build      # compiles ./library and marks the scripts executable
make clean      # removes the binary and cleans up IPC resources (/tmp/lib_cmd*, /tmp/catalog*.csv)
make run ARGS="3 csv_books/books.csv"   # bootstraps a scenario (3 libraries from books.csv)
```

`make run` simply forwards `ARGS` to `bootstrap.sh`.

---

## Quick start

```sh
# 1. Build
make build

# 2. Launch 3 libraries, splitting books.csv across them
./bootstrap.sh 3 csv_books/books.csv

# 3. Use the system
./user.sh Alice 1 register
./user.sh Alice 1 borrow "The Great Gatsby"
./user.sh Alice 1 return "The Great Gatsby"
./user.sh Charlie 1 search --by author "F. Scott Fitzgerald"

# 4. Inspect / shut down
./manage.sh list_books
./manage.sh list_users
./manage.sh status
./manage.sh stop
```

---

## Components

| File                | Role                                                                       |
|---------------------|----------------------------------------------------------------------------|
| `library.c`         | The library process: catalog, user/inter-library request handling, IPC.    |
| `library_types.h`   | Shared data structures (`Book`, `User`, `PendingRequest`, `Library`).      |
| `bootstrap.sh`      | Splits the source CSV into N catalogs and launches N libraries.            |
| `user.sh`           | User-facing client: register / search / borrow / return.                  |
| `manage.sh`         | Admin script: status / list_books / list_users / stop.                    |
| `Makefile`          | `build` / `clean` / `run` targets.                                         |
| `csv_books/books.csv` | Source catalog (`Title,Author,Year`, with a header row).                |

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
./user.sh Charlie 1 search --by author "F. Scott Fitzgerald"
./user.sh Charlie 1 search --by title  "1984"
./user.sh Charlie 1 search --by year   1984
```
Usernames are normalized to uppercase. A user must `register` with a library before
borrowing, and may hold at most one book at a time.

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
  `O_RDWR` so it never sees EOF when clients disconnect.
- A **detached worker thread per request** (user requests, inbound inter-library borrow
  requests, and management requests are each dispatched to their own thread), so requests are
  handled **concurrently**.
- The **main thread** which, after setup, blocks in `sigwait()` and handles `SIGUSR1`
  (status dump) and `SIGTERM` (clean shutdown) synchronously, making it safe to do file I/O
  and free resources without racing the worker threads.

### IPC
- **FIFOs** are the only IPC channel. Every library has one well-known command FIFO,
  `/tmp/lib_cmd_<id>`, that carries user requests, inter-library messages, and management
  requests.
- Clients pass a **response FIFO** path in their request; the library opens it and writes the
  reply back.
- Inter-library writes use `O_WRONLY | O_NONBLOCK` so a library never blocks (and never
  deadlocks against its own listener) when contacting a peer.

### Borrowing across libraries
1. A user asks library *A* to borrow a book.
2. If *A* owns the book, it lends it locally under the per-book lock.
3. If not, *A* broadcasts `LIB|REQUEST` to all peers and waits (with a **TTL**, see
   `BROADCAST_TIMEOUT_SEC`) on a `PendingRequest` slot.
4. Peers reply `GRANTED` / `ALREADY_LENT` / `NOT_FOUND`. The first peer that owns and can lend
   the book wins.
5. The user always **returns the book to the library they borrowed from**; that library then
   forwards a `LIB|RETURN` to the owning library.

### Synchronization
- One mutex **per book** and one **per user** guard the mutable state, so two users borrowing
  different books never contend.
- A global `users_lock` guards the user registry (including growth via `realloc`).
- `PendingRequest` slots use an atomic `in_use` flag plus a `request_id` check so that late or
  stale inter-library responses are safely ignored.

---

## Wire protocol (FIFO messages)

Messages are `|`-delimited records.

```
# User -> library
USER|REGISTER|<user>|<response_fifo>
USER|BORROW|<user>|<title>|<response_fifo>
USER|RETURN|<user>|<title>|<response_fifo>
USER|SEARCH|<user>|<field>|<value>|<response_fifo>      # field = author|title|year

# Library -> library
LIB|REQUEST|<src_id>|<req_id>|BORROW|<title>
LIB|RESPONSE|<responder_id>|<req_id>|<GRANTED|ALREADY_LENT|NOT_FOUND>
LIB|RETURN|<src_id>|<title>

# Management -> library
MGMT|LIST_BOOKS|<response_fifo>
MGMT|LIST_USERS|<response_fifo>
```

Responses to user requests are formatted as `<code>|<message>` (see below).

---

## Error / status codes

The library answers every user request with a leading numeric code; `user.sh` exits `0` on
code `0` and `1` otherwise.

| Code | Meaning                                                             |
|------|---------------------------------------------------------------------|
| 0    | Success (registered / lent / returned / search results)             |
| 1    | No such user, or book found in no catalog at all                    |
| 2    | User already registered, or invalid search field                    |
| 3    | No such book (on return)                                            |
| 4    | Book is already lent out                                            |
| 5    | User has no book to return                                          |
| 6    | System busy / allocation failure, retry                             |
| 7    | User already holds a book (must return before borrowing another)    |
| 8    | User does not hold that particular book                             |

---

## Cleanup

`make clean` and `./manage.sh stop` both remove the FIFOs (`/tmp/lib_cmd*`) and split catalogs
(`/tmp/catalog*.csv`). On `SIGTERM` each library also unlinks its own command FIFO and frees
its catalog, user registry, and mutexes before exiting.
