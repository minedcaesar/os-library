#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "library_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>


Library lib;

// Utility funcs
int request_id();
User* find_user(const char*);
Book* find_book(const char*);
void send_message(char*, int);
int count_lines(char[]);
Book* read_catalog(char*, int);
void register_user(char*, int);
void borrow_book(char*, char*, int);
void return_book(char*, char*, int);
void search_book();
void* user_request_thread(void*);
int lib_request(int, enum Outcome*, int*, const char*, ...)
    __attribute__((format(printf, 4, 5)));
void* borrow_request_thread(void*);
void handle_user_message(char*);
void handle_library_request(char*);
void process_message(char*);
void* listener_thread(void*);
void handle_mgmt_message(char*);
void handle_pending(int, char *, int);
void *mgmt_request_thread(void *);
int matches(const Book *, int, int, int, const char *);
char *unquote(char*);

int main(int argc, char *argv[]) {
    if (argc != 4)
    {
        fprintf(stderr, "Usage: %s <library_id> <num_libraries> <catalog_file>\n", argv[0]);
        return 1;
    }

    srand(time(NULL));
    for (int i = 0; i < MAX_PENDING; i++) {
        pthread_mutex_init(&lib.pending[i].lock, NULL);
        pthread_cond_init(&lib.pending[i].cond, NULL);
        atomic_store(&lib.pending[i].in_use, 0);
    }

    // 1. Block Management Signals Early
    // Threads inherit the signal mask of their creator. By blocking SIGUSR1 and SIGTERM 
    // here, we guarantee that no random background worker thread gets interrupted by them.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
        perror("pthread_sigmask failed");
        return 1;
    }

    // 2. Ignore SIGPIPE Safely
    // Prevents the library process from crashing if a client closes their response pipe early
    signal(SIGPIPE, SIG_IGN); 

    // 3. Initialize Library Metadata & Catalog
    lib.id = atoi(argv[1]);
    lib.num_total_libraries = atoi(argv[2]);
    lib.next_id = 0;
    char *catalog_file = argv[3];
    
    lib.num_books = count_lines(catalog_file);
    if (lib.num_books == -1) {
        perror("Cannot read CSV catalog file");
        return -1;
    }
    
    lib.catalog = read_catalog(catalog_file, lib.num_books);
    if (!lib.catalog)
    {
        fprintf(stderr, "Failed to load catalog into memory\n");
        return 1;
    }

    // 4. Initialize User Registry
    lib.capacity = 50;
    lib.users = malloc(lib.capacity * sizeof(User *));
    if (!lib.users) {
        perror("Failed to allocate initial users array");
        free(lib.catalog);
        return 1;
    }
    lib.num_users = 0;
    pthread_mutex_init(&lib.users_lock, NULL);

    // 5. Setup the Incoming Command FIFO Command Channel
    snprintf(lib.pipe_path, sizeof(lib.pipe_path), "/tmp/lib_cmd_%d", lib.id);
    
    // Clean up an old pipe node if it was left over from a crash
    unlink(lib.pipe_path); 
    if (mkfifo(lib.pipe_path, 0666) < 0) {
        perror("mkfifo failed for incoming command pipeline");
        free(lib.users);
        free(lib.catalog);
        return 1;
    }

    // Open in Read/Write mode so our listener thread doesn't read a constant 0 (EOF) 
    // when individual client shell scripts disconnect.
    lib.pipe_fd = open(lib.pipe_path, O_RDWR);
    if (lib.pipe_fd < 0) {
        perror("Failed to open command FIFO");
        unlink(lib.pipe_path);
        free(lib.users);
        free(lib.catalog);
        return 1;
    }

    // 6. Spawn the Background Listener Thread
    // This thread handles incoming commands (BORROW, MGMT|LIST_BOOKS, MGMT|LIST_USERS)
    if (pthread_create(&lib.listener_thread, NULL, listener_thread, NULL) != 0) {
        perror("pthread_create failed for listener_thread");
        close(lib.pipe_fd);
        unlink(lib.pipe_path);
        free(lib.users);
        free(lib.catalog);
        return 1;
    }

    // 7. Synchronous Management Loop (Main Thread)
    // Instead of jumping into thread logs, the main thread sleeps right here until manage.sh 
    // signals it. Because it wakes up synchronously, it is 100% safe to do file I/O and locks.
    int sig;
    while (1) {
        // Blocks until SIGUSR1 or SIGTERM arrives for this process
        sigwait(&set, &sig); 

        if (sig == SIGUSR1) {
            // FUNCTION 1: Dump structural node diagnostics
            char filename[256];
            snprintf(filename, sizeof(filename), "library_status_%d.txt", lib.id);

            FILE *fp = fopen(filename, "w");
            if (fp) {
                fprintf(fp, "=== Library Node [%d] ===\n", lib.id);
                fprintf(fp, "Status: Operational / Running\n");
                fprintf(fp, "Active Clients Registered: %d\n", lib.num_users);
                fprintf(fp, "Catalog Volume Size: %d\n\n", lib.num_books);
                fclose(fp);
            }
        } 
        else if (sig == SIGTERM) {
            // FUNCTION 4: Clean up IPC system resources and shut down
            printf("[Library %d] Received shutdown signal. Cleaning up...\n", lib.id);
            
            // Critical step: Dropping the file system link address off the machine layout
            close(lib.pipe_fd);
            unlink(lib.pipe_path);
            
            // Clean up allocated memory space
            pthread_mutex_lock(&lib.users_lock);
            for(int i = 0; i < lib.num_users; i++) {
                pthread_mutex_destroy(&lib.users[i]->lock);
                free(lib.users[i]);
            }
            free(lib.users);
            pthread_mutex_unlock(&lib.users_lock);
            pthread_mutex_destroy(&lib.users_lock);

            for(int i = 0; i < lib.num_books; i++) {
                pthread_mutex_destroy(&lib.catalog[i].lock);
            }
            free(lib.catalog);
            
            printf("[Library %d] Resources freed. System offline.\n", lib.id);
            exit(0);
        }
    }

    return 0; // Never reached logically
}
// Opens a peer library's command FIFO, writes msg, and closes it. Non-blocking, so a
// busy or dead peer never stalls us (and never deadlocks against our own listener).
// Returns 0 on success, -1 if the peer is unreachable.
int send_to_library(int lib_id, const char *msg) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/lib_cmd_%d", lib_id);
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    write(fd, msg, strlen(msg));
    close(fd);
    return 0;
}

// Issues an inter-library request and blocks until a verdict arrives or
// BROADCAST_TIMEOUT_SEC elapses (TTL). The wire message is
// "LIB|REQUEST|<self>|<id>|<payload>\n": the function owns the header, id and framing
// (so responses route back correctly), while the caller crafts the payload via fmt/...
// e.g. lib_request(BROADCAST_ALL, &o, &r, "BORROW|%s", title).
//   target    : a specific peer id, or BROADCAST_ALL to contact every other library.
//   outcome   : out — LENT / ALREADY_LENT, or PENDING if it timed out / nobody answered.
//   responder : out — id that produced the verdict (meaningful for LENT / ALREADY_LENT).
// Returns 0 on success, or -1 if the pending-request pool is exhausted.
int lib_request(int target, enum Outcome *outcome, int *responder,
                const char *fmt, ...) {
    int id = request_id();
    if (id < 0)
        return -1;

    char payload[2000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(payload, sizeof(payload), fmt, ap);
    va_end(ap);

    char message[2048];
    snprintf(message, sizeof(message), "LIB|REQUEST|%d|%d|%s\n", lib.id, id, payload);

    PendingRequest *ptr = &lib.pending[id % MAX_PENDING];
    pthread_mutex_lock(&ptr->lock);

    if (target == BROADCAST_ALL) {
        ptr->num_requests = lib.num_total_libraries - 1;
        for (int i = 1; i <= lib.num_total_libraries; i++) {
            if (i == lib.id)
                continue;
            if (send_to_library(i, message) < 0)
                ptr->received_responses++;   // unreachable peer counts as already answered
        }
    } else {
        ptr->num_requests = 1;
        if (send_to_library(target, message) < 0)
            ptr->received_responses++;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += BROADCAST_TIMEOUT_SEC;

    while (ptr->outcome == PENDING && ptr->received_responses < ptr->num_requests) {
        if (pthread_cond_timedwait(&ptr->cond, &ptr->lock, &deadline) == ETIMEDOUT)
            break;
    }

    *outcome   = ptr->outcome;
    *responder = ptr->response_id;
    atomic_store(&ptr->in_use, 0);
    pthread_mutex_unlock(&ptr->lock);
    return 0;
}
char *unquote(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = '\0';
        return s + 1;
    }
    return s;
}

// look up request matrix for free cell
int request_id(void) {
    for (int attempt = 0; attempt < MAX_PENDING; attempt++) {
        int full_id = atomic_fetch_add(&lib.next_id, 1);
        int idx = full_id % MAX_PENDING;
        int expected = 0;
        if (atomic_compare_exchange_strong(&lib.pending[idx].in_use, &expected, 1)) {
            pthread_mutex_lock(&lib.pending[idx].lock);
            lib.pending[idx].received_responses = 0;
            lib.pending[idx].request_id = full_id;
            lib.pending[idx].outcome = PENDING;
            pthread_mutex_unlock(&lib.pending[idx].lock);
            return full_id;
        }
    }
    return -1;   // pool exhausted, caller must handle
}

User *find_user(const char *username) {
    // assumes the lib.users lock is already held by caller
    User *user_ptr = NULL;
    for (int i = 0; i < lib.num_users; i++)
    {
        if (strcmp(lib.users[i]->username, username) == 0)
        {
            user_ptr = lib.users[i];
            break;
        }
    }
    return user_ptr;
}

Book* find_book(const char* title) {
    Book *book_ptr = NULL;
    for (int i = 0; i < lib.num_books; i++)
    {
        if (strcmp(lib.catalog[i].name, title) == 0)
        {
            book_ptr = &lib.catalog[i];
            break;
        }
    }
    return book_ptr;
}

void send_message(char *message, int fd) {
    // created this so you wouldnt need to manually create a variable and calculate the length of a variable each time
    write(fd, message, strlen(message));
}

int count_lines(char file_name[]) {
    FILE *fp = fopen(file_name, "r");
    if (!fp) {
        perror(file_name);
        return -1;
    }
    int count = 0;
    int c, last = '\n';
    for (c = getc(fp); c != EOF; c = getc(fp))
    {
        if (c == '\n')
        {
            count++;
        }
        last = c;
    }
    if(last!='\n' && last != EOF)
        count++;
    fclose(fp);
    return count;
}
//reads from a catalog txt file and returns a Book* catalog.
Book *read_catalog(char *catalog_file, int lines) {
    int MAX_FIELD_LENGTH = 1024;
    FILE *fp = fopen(catalog_file, "r");
    if (!fp)
        return NULL;
    Book *catalog = (Book *)malloc(lines * sizeof(Book));
    if (catalog == NULL){
        //needs to send some sort of error
        return NULL;
    }
    char row[MAX_FIELD_LENGTH];
    int current_book = 0;
    while (fgets(row, sizeof(row), fp) && current_book < lines)
    {

        row[strcspn(row, "\r\n")] = 0;

        if (strlen(row) == 0)
            continue;

        char *token = strtok(row, ",");
        if (token != NULL) {
            token = unquote(token);
            strncpy(catalog[current_book].name, token, sizeof(catalog[current_book].name) - 1);
            catalog[current_book].name[sizeof(catalog[current_book].name) - 1] = '\0';
        }
        token = strtok(NULL, ",");
        if (token != NULL) {
            token = unquote(token);
            strncpy(catalog[current_book].author, token, sizeof(catalog[current_book].author) - 1);
            catalog[current_book].author[sizeof(catalog[current_book].author) - 1] = '\0';
        }
        token = strtok(NULL, ",");
        if (token != NULL)
        {
            catalog[current_book].year = atoi(token);
            catalog[current_book].availability = AVAILABLE;
            catalog[current_book].lent_to[0] = '\0';
        }
        pthread_mutex_init(&catalog[current_book].lock, NULL); // initializing per book mutex
        current_book++;
    }
    fclose(fp);
    return catalog;
}
// adds user to lib.users
void register_user(char *username, int fd) {
    sleep(1 + rand() % 5);
    pthread_mutex_lock(&lib.users_lock);

    for (int i = 0; i < lib.num_users; i++)
    {
        if (strcmp(lib.users[i]->username, username) == 0)
        {
            perror("Username already taken");
            pthread_mutex_unlock(&lib.users_lock);
            send_message("2|User already registered", fd);
            return;
        }
    }

    if (lib.num_users == lib.capacity)
    {
        int newcap = lib.capacity * 2;
        User **tmp = realloc(lib.users, newcap * sizeof(User *));
        if (tmp == NULL)
        {
            perror("Failed to reallocate");
            pthread_mutex_unlock(&lib.users_lock);
            send_message("6|Failed to reallocate user list, please retry", fd);
            return;
        }
        lib.users = tmp;
        lib.capacity = newcap;
    }
    User *u = malloc(sizeof(User));
    if (!u)
    {
        pthread_mutex_unlock(&lib.users_lock);
        send_message("6|Malloc failed", fd);
        return;
    }
    strncpy(u->username, username, sizeof(u->username) - 1);
    u->username[sizeof(u->username) - 1] = '\0';
    u->borrowed[0] = '\0';
    pthread_mutex_init(&u->lock, NULL);
    u->borrowed_from_lib = -1;
    lib.users[lib.num_users++] = u;
    pthread_mutex_unlock(&lib.users_lock);
    send_message("0|User registered", fd);
}

// core operations
void borrow_book(char *username, char *book_title, int fd) {

    // Must lock users list to safely read and prevent realloc invalidation
    pthread_mutex_lock(&lib.users_lock);
    User *user_ptr = find_user(username);
    pthread_mutex_unlock(&lib.users_lock);

    if (user_ptr == NULL)
    {
        send_message("1|No such User.", fd);
        return;
    }
    pthread_mutex_lock(&user_ptr->lock);
    if (user_ptr->borrowed[0] != '\0')
    {
        send_message("7|User has a book", fd);
        pthread_mutex_unlock(&user_ptr->lock);
        return;
    }

    Book *book_ptr = find_book(book_title);
    /*
     * CASE A — Book is not in this library's catalog.
     *
     * Broadcast a BORROW request to every other library and wait
     * for a verdict (someone lent it / someone says it's already
     * out / nobody answers in time → assume the book doesn't exist even though they might have crashed).
     */
    if (book_ptr == NULL)
    {
        enum Outcome outcome;
        int lib_responder =-1;
        if (lib_request(BROADCAST_ALL, &outcome, &lib_responder, "BORROW|%s", book_title) < 0) {
            send_message("6|System busy, try again later", fd);
            pthread_mutex_unlock(&user_ptr->lock);
            return;
        }
              if (outcome == LENT) {
            strncpy(user_ptr->borrowed, book_title, sizeof(user_ptr->borrowed) - 1);
            user_ptr->borrowed[sizeof(user_ptr->borrowed) - 1] = '\0';
            user_ptr->borrowed_from_lib = lib_responder;
            send_message("0|Book has successfully been lent", fd);
        }
        else if (outcome == ALREADY_LENT) {
            send_message("4|Book was already lent to another user", fd);
        }
        else { // PENDING — timed out, or nobody had the book
            send_message("1|No such book.", fd);
        }
        pthread_mutex_unlock(&user_ptr->lock);
        return;
    }
    // CASE B — Book is in *this* library's catalog. Local borrow.
    
    sleep(1 + rand()%5);
    pthread_mutex_lock(&(book_ptr->lock));

    if (book_ptr->availability == LENT_OUT) {
        if (book_ptr->lent_to_lib == -1 || book_ptr->really_lent == 1) {
            send_message("4|Book not available", fd);                 // local or confirmed -> refuse
            pthread_mutex_unlock(&book_ptr->lock);
            pthread_mutex_unlock(&user_ptr->lock);
            return;
        }
        // unconfirmed remote loan -> verify without the lock
        int  target = book_ptr->lent_to_lib;
        char title[100], borrower[100];
        strncpy(title, book_ptr->name, sizeof(title)-1);       title[sizeof(title)-1]='\0';
        strncpy(borrower, book_ptr->lent_to, sizeof(borrower)-1); borrower[sizeof(borrower)-1]='\0';
        pthread_mutex_unlock(&book_ptr->lock);

        enum Outcome o; int who = -1;
        int rc = lib_request(target, &o, &who, "VERIFY|%s|%s", title, borrower);

        pthread_mutex_lock(&book_ptr->lock);
        // only apply the verdict if nothing changed under us
        if (book_ptr->availability == LENT_OUT &&
            book_ptr->lent_to_lib == target && book_ptr->really_lent == 0) {
            if (rc == 0 && o == NOT_HELD) {
                book_ptr->availability = AVAILABLE;   // reclaim, then fall through ↓
            } else {
                if (o == HELD) book_ptr->really_lent = 1;
                send_message("4|Book not available", fd);   
                pthread_mutex_unlock(&book_ptr->lock);
                pthread_mutex_unlock(&user_ptr->lock);
                return;
            }
        }
        // else: returned or resolved by another thread -> let the block below re-read availability
    }

    // 2. Single local-lend path: original AVAILABLE, reclaimed, or returned-while-we-waited.
    if (book_ptr->availability == AVAILABLE) {
        book_ptr->availability = LENT_OUT;
        book_ptr->lent_to_lib  = -1;        // it's a local loan now
        book_ptr->really_lent  = 0;
        strncpy(book_ptr->lent_to, username, sizeof(book_ptr->lent_to)-1);
        book_ptr->lent_to[sizeof(book_ptr->lent_to)-1]='\0';
        strncpy(user_ptr->borrowed, book_title, sizeof(user_ptr->borrowed)-1);
        user_ptr->borrowed[sizeof(user_ptr->borrowed)-1]='\0';
        send_message("0|Success", fd);
    } else {
        send_message("4|Book not available", fd);  // someone else re-lent it during our verify
    }
    pthread_mutex_unlock(&book_ptr->lock);
    pthread_mutex_unlock(&user_ptr->lock);
    return;

void return_book(char *username, char *book_title, int fd) {

    sleep(1 + rand()%5);
    pthread_mutex_lock(&lib.users_lock);
    User *user_ptr = find_user(username);
    pthread_mutex_unlock(&lib.users_lock);

    if (user_ptr == NULL)
    {
        perror("No such user");
        send_message("1|No such user", fd);
        return;
    }
    // check if they have no book
    pthread_mutex_lock(&user_ptr->lock);
    if ((user_ptr->borrowed)[0] == '\0')
    {
        send_message("5|User doesnt have a book", fd);
        pthread_mutex_unlock(&user_ptr->lock);
        return;
    }
    // check if they are trying to return the right book
    if (strcmp(user_ptr->borrowed, book_title))
    {
        send_message("8|User doesnt have that book", fd);
        pthread_mutex_unlock(&user_ptr->lock);
        return;
    }

    if(user_ptr->borrowed_from_lib!=-1){
        char buffer[1024];
        sprintf(buffer,"LIB|RETURN|%d|%s\n", lib.id, book_title);
        char path[1024];
        sprintf(path,"/tmp/lib_cmd_%d", user_ptr->borrowed_from_lib);
        int lib_fd = open(path, O_WRONLY | O_NONBLOCK);
        if(lib_fd<0){
            pthread_mutex_unlock(&user_ptr->lock);
            return;
        }
        send_message(buffer,lib_fd);
        close(lib_fd);
        user_ptr->borrowed[0] = '\0';
        user_ptr->borrowed_from_lib=-1; //problem, what happens if the library sends the message, but the other doesnt receive it
        //? the book will be permanently locked, but this is the two generals problem
        send_message("0|Successfully returned the book", fd);
        pthread_mutex_unlock(&user_ptr->lock);
        return;
    }

    Book *book_ptr = find_book(book_title);
    if (book_ptr == NULL)
    {
        send_message("3|No such book",fd);
        pthread_mutex_unlock(&user_ptr->lock);
        return;
    }
    else
    {

        pthread_mutex_lock(&book_ptr->lock);
        book_ptr->availability = AVAILABLE;
        strncpy(book_ptr->lent_to, "", sizeof(book_ptr->lent_to));
        strncpy(user_ptr->borrowed, "", sizeof(user_ptr->borrowed));
        user_ptr->borrowed[0]= '\0';
        pthread_mutex_unlock(&book_ptr->lock);
        pthread_mutex_unlock(&user_ptr->lock);
        send_message("0|Success", fd);
    }
}

int matches(const Book *b, int by_author, int by_title, int by_year, const char *value) {
    if (by_author) return strstr(b->author, value) != NULL;
    if (by_title)  return strstr(b->name,   value) != NULL;
    if (by_year) {
        char ys[16];
        snprintf(ys, sizeof(ys), "%d", b->year);
        return strstr(ys, value) != NULL;
    }
    return 0;
}

void search_book(char *username, char *field, char *value, int fd) {
    sleep(1 + rand()%5);
    (void)username;

    int by_author = (strcmp(field, "author") == 0);
    int by_title  = (strcmp(field, "title")  == 0);
    int by_year   = (strcmp(field, "year")   == 0);

    if (!by_author && !by_title && !by_year) {
        char err[128];
        snprintf(err, sizeof(err), "2|Invalid search field '%s'\n", field);
        send_message(err, fd);
        return;
    }

    int found = 0;
    for (int i = 0; i < lib.num_books && !found; i++) found = matches(&lib.catalog[i], by_author, by_title, by_year, value);

    if (!found) {
        char msg[256];
        snprintf(msg, sizeof(msg), "0|No books found matching %s = '%s'\n", field, value);
        send_message(msg, fd);
        return;
    }

    send_message("0|Search results:\n", fd);          // status line first
    for (int i = 0; i < lib.num_books; i++) {
        if (matches(&lib.catalog[i], by_author, by_title, by_year, value)) {
            char line[512];
            snprintf(line, sizeof(line), "%s by %s (%d)\n", lib.catalog[i].name, lib.catalog[i].author, lib.catalog[i].year);
            send_message(line, fd);                    // one write per match
        }
    }
}

// working threads
void *user_request_thread(void *arg) {
    UserRequestContext *ctx = (UserRequestContext *)arg;
    char *op = ctx->operation;
    int fd = open(ctx->response_pipe, O_WRONLY);
    if (fd < 0) {
        free(ctx);
        return NULL;
    }
    char *username = ctx->username;
    char *arg1 = ctx->arg1;
    if (strcmp(op, "REGISTER") == 0)
    {
        register_user(username, fd);
    }
    else if (strcmp(op, "BORROW") == 0)
    {
        borrow_book(username, arg1, fd);
    }
    else if (strcmp(op, "RETURN") == 0) {
        return_book(username, arg1, fd);
    }
    else if (strcmp(op, "SEARCH") == 0) {
        char *arg2 = ctx->arg2;
        search_book(username, arg1, arg2, fd);
    }
    close(fd);
    free(arg);
    return NULL;
}

void *borrow_request_thread(void *arg) {
    LibraryRequestContext *ctx = (LibraryRequestContext *)arg;

    // Specification: random 1-5 second delay before responding
    sleep(1 + rand() % 5);

    const char *outcome_str;
    Book *book_ptr = find_book(ctx->book_title);

    if (book_ptr == NULL) {
        outcome_str = "NOT_FOUND";
    } else {
        pthread_mutex_lock(&book_ptr->lock);
        if (book_ptr->availability == AVAILABLE) {
            book_ptr->availability= LENT_OUT;
            book_ptr->lent_to[0] = 'ctx.';      // add name of the borrower
            outcome_str = "GRANTED";
            book_ptr->lent_to_lib = ctx->src_lib;
            book_ptr->really_lent = 0;
        } else {
            outcome_str = "ALREADY_LENT"; // should try to recover?
        }
        pthread_mutex_unlock(&book_ptr->lock);
    }

    char response[512];
    snprintf(response, sizeof(response), "LIB|RESPONSE|%d|%d|%s\n", lib.id, ctx->request_id, outcome_str);

    char path[256];
    snprintf(path, sizeof(path), "/tmp/lib_cmd_%d", ctx->src_lib);
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        send_message(response, fd);
        close(fd);
    }
    // if open fails the peer is dead; they'll time out, nothing we can do

    free(ctx);
    return NULL;
}


void *verify_request_thread(void *arg){
    VerifyContext *vericontext = (VerifyContext *)arg;
    pthread_mutex_lock(&lib.users_lock);
    User *user_ptr = find_user(vericontext->username);
    pthread_mutex_unlock(&lib.users_lock);
    char message[1024];
    if(user_ptr==NULL){
        sprintf(message, sizeof(message), "LIB|RESPONSE|%d|%d|%s\n", lib.id, vericontext->request_id, "NOT_HELD");
    }
    else{
        pthread_mutex_lock(&user_ptr->lock);
        if(strcmp(user_ptr->borrowed,vericontext->book_title)==0){
            snrintf(message, sizeof(message), "LIB|RESPONSE|%d|%d|%s\n", lib.id, vericontext->request_id, "HELD");
        }
        else{
            snprintf(message, sizeof(message), "LIB|RESPONSE|%d|%d|%s\n", lib.id, vericontext->request_id, "NOT_HELD");
        }
    }
    send_to_library(vericontext->src_lib, message);
    pthread_mutex_unlock(&user_ptr->lock);
}
// message handling
void handle_user_message(char *message) {
    UserRequestContext *ctx = calloc(1, sizeof(*ctx)); // calloc -> all fields zeroed, so no need to do = '\0'
    if (!ctx)
    {
        perror("calloc");
        return;
    }

    char msg_copy[4096];
    size_t len = strnlen(message, sizeof(msg_copy) - 1);
    memcpy(msg_copy, message, len);
    msg_copy[len] = '\0';

    char *save = NULL;
    char *token;

    // "USER" header — already verified by process_message, just consume it
    token = strtok_r(msg_copy, "|", &save);
    if (!token || strcmp(token, "USER") != 0)
    {
        free(ctx);
        return;
    }

    // operation
    token = strtok_r(NULL, "|", &save);
    if (!token)
    {
        free(ctx);
        return;
    }
    strncpy(ctx->operation, token, sizeof(ctx->operation) - 1);

    // username
    token = strtok_r(NULL, "|", &save);
    if (!token)
    {
        free(ctx);
        return;
    }
    strncpy(ctx->username, token, sizeof(ctx->username) - 1);

    if (strcmp(ctx->operation, "REGISTER") == 0)
    {
        // USER|REGISTER|username|response_pipe
        token = strtok_r(NULL, "|", &save);
        if (!token)
        {
            free(ctx);
            return;
        }
        strncpy(ctx->response_pipe, token, sizeof(ctx->response_pipe) - 1);
    }
    else if (strcmp(ctx->operation, "BORROW") == 0 ||
            strcmp(ctx->operation, "RETURN") == 0)
    {
        // USER|<OP>|username|book_title|response_pipe
        token = strtok_r(NULL, "|", &save);
        if (!token)
        {
            free(ctx);
            return;
        }
        strncpy(ctx->arg1, token, sizeof(ctx->arg1) - 1);

        token = strtok_r(NULL, "|", &save);
        if (!token)
        {
            free(ctx);
            return;
        }
        strncpy(ctx->response_pipe, token, sizeof(ctx->response_pipe) - 1);
    }
    else if (strcmp(ctx->operation, "SEARCH") == 0)
    {
        // USER|SEARCH|username|field|value|response_pipe
        token = strtok_r(NULL, "|", &save);
        if (!token)
        {
            free(ctx);
            return;
        }
        strncpy(ctx->arg1, token, sizeof(ctx->arg1) - 1);

        token = strtok_r(NULL, "|", &save);
        if (!token)
        {
            free(ctx);
            return;
        }
        strncpy(ctx->arg2, token, sizeof(ctx->arg2) - 1);

        token = strtok_r(NULL, "|", &save);
        if (!token)
        {
            free(ctx);
            return;
        }
        strncpy(ctx->response_pipe, token, sizeof(ctx->response_pipe) - 1);
    }
    else
    {
        free(ctx); // unknown op — don't leak
        return;
    }

    pthread_t thread;
    if (pthread_create(&thread, NULL, user_request_thread, ctx) != 0)
    {
        perror("pthread_create");
        free(ctx);
        return;
    }
    pthread_detach(thread);
}

void handle_library_request(char *message) {
    char msg_copy[4096];
    size_t len = strnlen(message, sizeof(msg_copy) - 1);
    memcpy(msg_copy, message, len);
    msg_copy[len] = '\0';

    char *save = NULL;
    strtok_r(msg_copy, "|", &save);
    char *kind = strtok_r(NULL, "|", &save);
    if (!kind) return;

    if (strcmp(kind, "REQUEST") == 0) {
        // LIB|REQUEST|<src_lib>|<id>|BORROW|<title>
        char *src_s   = strtok_r(NULL, "|", &save);
        char *id_s    = strtok_r(NULL, "|", &save);
        char *op      = strtok_r(NULL, "|", &save);
        char *title   = strtok_r(NULL, "|", &save);
        if (!src_s || !id_s || !op || !title) return;
        if (strcmp(op, "BORROW") == 0) {
            // LIB|REQUEST|<src>|<id>|BORROW|<title>
            LibraryRequestContext *ctx = calloc(1, sizeof(*ctx));
            if (!ctx) return;
            ctx->src_lib    = atoi(src_s);
            ctx->request_id = atoi(id_s);
            strncpy(ctx->book_title, title, sizeof(ctx->book_title) - 1);
            pthread_t t;
            if (pthread_create(&t, NULL, borrow_request_thread, ctx) != 0) { free(ctx); return; }
            pthread_detach(t);
        }
        else if (strcmp(op, "VERIFY") == 0) {
            // LIB|REQUEST|<src>|<id>|VERIFY|<title>|<user>   (VERIFY carries an extra field)
            char *user = strtok_r(NULL, "|", &save);
            if (!user) return;
            VerifyContext *ctx = calloc(1, sizeof(*ctx));
            if (!ctx) return;
            ctx->src_lib    = atoi(src_s);
            ctx->request_id = atoi(id_s);
            strncpy(ctx->book_title, title, sizeof(ctx->book_title) - 1);
            strncpy(ctx->username,   user,  sizeof(ctx->username)  - 1);
            pthread_t t;
            if (pthread_create(&t, NULL, verify_request_thread, ctx) != 0) { free(ctx); return; }
            pthread_detach(t);
        }
    }
    else if (strcmp(kind, "RESPONSE") == 0) {
        // LIB|RESPONSE|<responder_lib>|<id>|<outcome>
        char *responder_s = strtok_r(NULL, "|", &save);
        char *id_s        = strtok_r(NULL, "|", &save);
        char *outcome     = strtok_r(NULL, "|", &save);
        if (!responder_s || !id_s || !outcome) return;

        handle_pending(atoi(id_s), outcome, atoi(responder_s));
    }
    else if (strcmp(kind, "RETURN") == 0) {
        // LIB|RETURN|<src_lib>|<title>
        char *src_s = strtok_r(NULL, "|", &save);
        char *title = strtok_r(NULL, "|", &save);
        if (!src_s || !title) return;

        Book *book_ptr = find_book(title);
        if (!book_ptr) return;     // not our book; nothing to do

        pthread_mutex_lock(&book_ptr->lock);
        book_ptr->availability = AVAILABLE;
        book_ptr->lent_to[0] = '\0';
        pthread_mutex_unlock(&book_ptr->lock);
    }
    else if (strcmp(kind, "VERIFY") == 0){
        //LIB|VERIFY|src_lib|id|BOOK_NAME|USER_NAME


    }
}

void handle_pending(int id,char *response,int lib_id){ // Gets called by the listenerthread to handle responses to pending requests.
    int pending_slot = id % MAX_PENDING;
    PendingRequest *ptr = &lib.pending[pending_slot];
    pthread_mutex_lock(&ptr->lock); 
    if (!atomic_load(&ptr->in_use) || ptr->request_id!=id){ //stale?
        pthread_mutex_unlock(&ptr->lock);
        return;
    }

    ptr->received_responses++;

    if(strcmp(response,"GRANTED")==0){
        ptr->response_id = lib_id;
        ptr->outcome = LENT;
    }
    if(strcmp(response,"ALREADY_LENT")==0){
        ptr->response_id = lib_id;
        ptr->outcome = ALREADY_LENT;
    }
    if(strcmp(response,"HELD")==0){
        ptr->response_id = lib_id;
        ptr->outcome = HELD;
    }
    if(strcmp(response,"NOT_HELD")==0){
        ptr->response_id = lib_id;
        ptr->outcome = NOT_HELD;
    }
    if(ptr->outcome!=PENDING || ptr->received_responses>=ptr->num_requests){
        pthread_cond_signal(&ptr->cond);
    }
    pthread_mutex_unlock(&ptr->lock);
}

//listening
void process_message(char *buffer) {
    char source[32];
    if (sscanf(buffer, "%31[^|]", source) == 1) {
        if (strcmp(source, "USER") == 0)     { handle_user_message(buffer); }
        if (strcmp(source, "LIB") == 0)  { handle_library_request(buffer); }
        if (strcmp(source, "MGMT") == 0) {
            char *msg = strdup(buffer); //need to dupe because buffer is a ptr to the stack of the listener thread
            if (!msg) return;

            pthread_t t;
            if (pthread_create(&t, NULL, mgmt_request_thread, msg) != 0) {
                perror("pthread_create mgmt");
                free(msg);
                return;
            }
            pthread_detach(t);
        }
    }
}

void *listener_thread(void *arg) {
    (void)arg;
    char buffer[4096];

    while (1)
    {
        ssize_t n = read(lib.pipe_fd, buffer, sizeof(buffer) - 1);
        if (n <= 0)
        {
            if (n == 0)
                continue; // all writers closed; loop and retry
            if (errno == EINTR)
                continue; // reading got interrupted by signal
            perror("read");
            break;
        }
        buffer[n] = '\0';

        char *outer_save = NULL;
        for (char *line = strtok_r(buffer, "\n", &outer_save);
            line != NULL;
            line = strtok_r(NULL, "\n", &outer_save))
        {
            process_message(line);
        }
    }
    return NULL;
}


void * mgmt_request_thread(void * arg){
    char *msg = (char *)arg;
    handle_mgmt_message(msg);
    free(msg);
    return NULL;
}

//READ THIS
void handle_mgmt_message(char * message) {
    char msg_copy[4096];
    strncpy(msg_copy, message, sizeof(msg_copy) - 1);
    msg_copy[sizeof(msg_copy) - 1] = '\0';

    char *save = NULL;
    strtok_r(msg_copy, "|", &save); // Skip "MGMT"
    char *op = strtok_r(NULL, "|", &save);  // Operation
    char *res_pipe = strtok_r(NULL, "|", &save);   // path of the response pipe

    if (!op || !res_pipe) return;

    // --- FEATURE 2: Real-time book catalog parsing ---
    if (strcmp(op, "LIST_BOOKS") == 0) {
        int fd = open(res_pipe, O_WRONLY);
        if (fd < 0) return;

        char buffer[2048];
        snprintf(buffer, sizeof(buffer), "--- Book Catalog for Library %d ---\n", lib.id);
        write(fd, buffer, strlen(buffer));

        for (int i = 0; i < lib.num_books; i++) {
            pthread_mutex_lock(&lib.catalog[i].lock);
            snprintf(buffer, sizeof(buffer), "  Book: \"%s\" by %s (%d) | Status: %s\n",
                    lib.catalog[i].name, lib.catalog[i].author, lib.catalog[i].year,
                    (lib.catalog[i].availability == AVAILABLE) ? "AVAILABLE" : "LENT OUT");
            pthread_mutex_unlock(&lib.catalog[i].lock);
            write(fd, buffer, strlen(buffer));
        }
        close(fd);
    }
    // --- FEATURE 3: Client indexes and borrows status ---
    else if (strcmp(op, "LIST_USERS") == 0) {
        int fd = open(res_pipe, O_WRONLY);
        if (fd < 0) return;

        char buffer[2048];
        snprintf(buffer, sizeof(buffer), "=== Library %d Registered Users ===\n", lib.id);
        write(fd, buffer, strlen(buffer));

        pthread_mutex_lock(&lib.users_lock);
        for (int i = 0; i < lib.num_users; i++) {
            pthread_mutex_lock(&lib.users[i]->lock);
            char state[256];
            if (lib.users[i]->borrowed[0] == '\0') {
                snprintf(state, sizeof(state), "No books borrowed");
            } else {
                snprintf(state, sizeof(state), "Borrowed: \"%s\"", lib.users[i]->borrowed);
            }
            snprintf(buffer, sizeof(buffer), "  User: %s | %s\n", lib.users[i]->username, state);
            pthread_mutex_unlock(&lib.users[i]->lock);
            write(fd, buffer, strlen(buffer));
        }
        pthread_mutex_unlock(&lib.users_lock);
        close(fd);
    }
}
