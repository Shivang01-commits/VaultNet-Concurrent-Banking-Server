# VaultNet-Concurrent-Banking-Server

A multi-client TCP banking server written in C for Linux. It supports four user roles, keeps all
data in binary record files, and stays consistent when processes crash.

## Features

| Role | What they can do |
|---|---|
| **Customer** | check balance, deposit, withdraw, transfer, apply for a loan, view own loans, view transaction history, send feedback, change password |
| **Employee** | add customers, modify customer details, view assigned loans, approve or reject loans, view a customer's passbook, change password |
| **Manager** | activate or deactivate customer accounts, assign loan applications to employees, review feedback, change password |
| **Admin** | add employees and managers, modify user details, manage roles, change password |

* Menu-driven client: after login, each user sees only the options their role allows.
* One session per user: a second login for the same user is refused until the first one ends.

## Architecture

```
client --TCP--> supervisor --> event loops (epoll) --> worker processes --> data files
```

* **Supervisor** accepts connections, passes each socket to the least busy loop
  (`SCM_RIGHTS`) and restarts any process that dies.
* **Event loop** holds the client connections with `epoll`, enforces one session per user
  and hands requests to its workers. It also applies idle timeouts and a worker watchdog.
* **Worker** is a stateless process that runs the banking operation. A crashed worker is
  replaced and the operation is recovered from the journal.
* **Data files** are fixed-size binary records in one folder: users, accounts, transactions,
  loans, feedback and a username index.

Everything is one executable (`bank_server`); the supervisor, loops and workers are
the same program started in different roles with `fork`.

## How consistency is kept (ACID)

* **Record locking:** `fcntl` locks on single records, always taken in a fixed order, so
  deadlocks cannot happen.
* **Redo journal:** a change is written to the journal and flushed, marked committed, and
  only then copied into the data files. A crash before the commit mark leaves no trace.
  A crash after it is finished by the next process that touches the same records.
* **Rules under lock:** balance, role and frozen-account checks happen while the records
  are locked.
* **One session per user:** a kernel lock on the user's byte in `sessions.dat`. It is
  released by the kernel when a client logs out, disconnects or dies, so a crash never
  leaves someone stuck "logged in".

## Performance design

* Each account keeps a chain of its own passbook lines, so a history page reads only the
  lines it shows instead of scanning all transactions (about 2 microseconds with 300,000
  lines in the file).
* Usernames are looked up through a hash index (`names.dat`), about 1 microsecond with
  50,000 users. The index entry is written in the same transaction as the user record, so
  usernames are unique and the index never disagrees with the users file.

## Project layout

| Path | Contents |
|---|---|
| `include/` | headers: record layouts, protocol, journal, server and client APIs |
| `src/db.c`, `src/journal.c` | record files, locking, transactions, crash recovery |
| `src/bank_*.c`, `src/auth.c` | banking functions (no networking), password hashing |
| `src/server/` | supervisor, event loop, worker, request dispatch |
| `src/client/` | client library |
| `src/proto.c` | fixed 512-byte packet protocol shared by client and server |
| `tools/` | `bank_server`, `bank_client` (menu client), `bank_cmd` (typed commands), `bankctl` (works on the files directly) |
| `tests/` | automated tests |

## Build and run

Requires Linux, `gcc`, `make` and `libcrypt`.

```bash
make                  # build everything into bin/
make test             # run the automated tests

ulimit -n 65536
./bin/bank_server --port 9090 --data ./data --loops 2 --workers 8 -v
./bin/bank_client     # in another terminal; connects to 127.0.0.1:9090
```

The first start creates the administrator `admin` / `admin123`. Log in as the admin to
add an employee and a manager, then use the employee to add customers.

Server options: `--port`, `--data`, `--loops`, `--workers`, `--idle SECS`,
`--worker-timeout SECS`, `--max-clients N`, `-v` (log connections and logins).

## Tests

`make test` builds and runs nine test programs. They crash the system on purpose at every
step of a commit, kill workers, event loops and the supervisor of a live server, and check
that balances, passbooks and sessions stay correct.

## Known limits

* Passwords are stored as salted hashes but travel as plain text over TCP (no TLS).
* If a worker crashes during a money operation, the client is told the result is unknown;
  the passbook shows what actually happened.
* At most about 126 requests run at the same time (one journal slot per worker); more
  requests wait in a queue. Connected clients are not limited by this.
* The username index has a fixed size of 262,144 buckets.
* Pending-loan and assigned-loan lists scan the loan file.
* Not implemented: closing an account, loan repayment and interest.

