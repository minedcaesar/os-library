#ifndef LIBRARY_TYPES_H
#define LIBRARY_TYPES_H

#include <pthread.h>
#include <stdatomic.h>

#define MAX_PENDING 2048
#define BROADCAST_TIMEOUT_SEC 8 // so smaller than user.sh timeout
#define VERIFY_TIMEOUT_SEC 4


#define BROADCAST_ALL 0   /* target id meaning "contact every other library" (ids start at 1) */
// book status
enum Availability
{
    AVAILABLE,
    LENT_OUT
};

// request to another lib
enum Outcome
{
    PENDING,
    LENT,
    ALREADY_LENT,
    HELD,        // verify: borrower confirms it really holds the book
    NOT_HELD,    // verify: borrower doesn't have it -> safe to reclaim
};

// user entity
typedef struct
{
    char username[100];
    char borrowed[100];
    int borrowed_from_lib;
    pthread_mutex_t lock;
} User;

// book entity
typedef struct
{
    char name[100];
    char author[100];
    int year;
    int lent_to_lib;
    int really_lent;
    enum Availability availability;
    char lent_to[100];
    pthread_mutex_t lock;
} Book;

typedef struct
{
    atomic_int in_use;
    int request_id;
    int response_id;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    enum Outcome outcome;
    int num_requests;
    int received_responses;
} PendingRequest;

typedef struct
{
    int id;
    Book *catalog;
    int num_books;
    User **users; // pointer of the vector of users
    int num_users;
    int capacity;
    pthread_mutex_t users_lock;
    int pipe_fd;
    char pipe_path[256];
    PendingRequest pending[MAX_PENDING];
    atomic_int next_id; // unique id for requests
    pthread_t listener_thread;
    int num_total_libraries;
} Library;

typedef struct
{
    char operation[32];
    char username[100];
    char arg1[512];
    char arg2[512];
    char response_pipe[256];
} UserRequestContext;

// Shared by BORROW and VERIFY inter-library requests: both carry the borrowing user, so the
// owner can record who holds a remotely-lent book and later verify it by name.
typedef struct
{
    int src_lib;
    int request_id;
    char book_title[256];
    char username[100];
} LibraryRequestContext;

typedef struct
{
    int src_lib;
    int request_id;
    char field[32];
    char value[512];
    char resp_pipe[256];   // the *user's* response FIFO — peers stream matches straight to it
} SearchContext;
extern Library lib;

#endif