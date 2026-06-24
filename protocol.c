#include "library.h"

// ---- IPC primitives -------------------------------------------------------

void send_message(char *message, int fd) {
    // created this so you wouldnt need to manually create a variable and calculate the length of a variable each time
    write(fd, message, strlen(message));
}

// Emits a user-facing status line "<code>|<message>" on fd. The numeric code comes from
// errors.h and is the single source of truth; user.sh reads field 1 and maps the same
// values. Keeping the code a real integer argument (rather than baked into the string)
// is what makes the error set "defined and used consistently" across C and Bash.
void send_status(int fd, int code, const char *message) {
    char buf[640];
    snprintf(buf, sizeof(buf), "%d|%s", code, message);
    send_message(buf, fd);
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

// Issues an inter-library request and blocks until a verdict arrives or
// BROADCAST_TIMEOUT_SEC elapses (TTL). The wire message is
// "LIB|REQUEST|<self>|<id>|<payload>\n": the function owns the header, id and framing
// (so responses route back correctly), while the caller crafts the payload via fmt/...
// e.g. lib_request(BROADCAST_ALL, &o, &r, "BORROW|%s", title).
//   target    : a specific peer id, or BROADCAST_ALL to contact every other library.
//   outcome   : out — LENT / ALREADY_LENT, or PENDING if it timed out / nobody answered.
//   responder : out — id that produced the verdict (meaningful for LENT / ALREADY_LENT).
// Returns 0 on success, or -1 if the pending-request pool is exhausted.
int lib_request(int target, enum Outcome *outcome, int *responder, const char *fmt, ...) {  // variable number of args
    int id = request_id();
    if (id < 0)
        return -1;

    char payload[2000];
    va_list ap;    // variable lenght list of args data structure
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

// Gets called by the listener thread to handle responses to pending requests.
void handle_pending(int id,char *response,int lib_id){
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

// ---- worker threads -------------------------------------------------------

// Handles an inter-library SEARCH: write our matching books straight to the requesting user's
// response pipe, then ack DONE so the requester's pending quorum can complete.
void *search_request_thread(void *arg) {
    SearchContext *ctx = (SearchContext *)arg;

    int by_author = (strcmp(ctx->field, "author") == 0);
    int by_title  = (strcmp(ctx->field, "title")  == 0);
    int by_year   = (strcmp(ctx->field, "year")   == 0);

    // Open non-blocking so we fail fast if the user already gave up (no reader); then drop
    // O_NONBLOCK so the result writes block rather than lose data.
    int fd = open(ctx->resp_pipe, O_WRONLY | O_NONBLOCK);   // the *user's* pipe, by path
    if (fd >= 0) {
        int fl = fcntl(fd, F_GETFL);
        if (fl != -1) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        for (int i = 0; i < lib.num_books; i++) {
            if (matches(&lib.catalog[i], by_author, by_title, by_year, ctx->value)) {
                char line[512];
                snprintf(line, sizeof(line), "%s by %s (%d)\n", lib.catalog[i].name, lib.catalog[i].author, lib.catalog[i].year);
                send_message(line, fd);
            }
        }
        close(fd);
    }

    // Ack DONE so the requester's pending quorum completes instead of waiting the full TTL.
    char done[128];
    snprintf(done, sizeof(done), "LIB|RESPONSE|%d|%d|DONE\n", lib.id, ctx->request_id);
    send_to_library(ctx->src_lib, done);

    free(ctx);
    return NULL;
}

void *user_request_thread(void *arg) {
    UserRequestContext *ctx = (UserRequestContext *)arg;
    char *op = ctx->operation;
    int fd = open(ctx->response_pipe, O_WRONLY);
    if (fd < 0) {
        free(ctx);
        return NULL;
    }
    // Spec 2.6: one random 1-5s processing delay per user-facing request, applied here so
    // inter-library messages (BORROW/VERIFY) stay fast and nested waits don't compound.
    sleep(1 + rand() % 5);
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
        search_book(username,arg1, arg2, fd,ctx->response_pipe);
    }
    close(fd);
    free(arg);
    return NULL;
}

void *borrow_request_thread(void *arg) {
    LibraryRequestContext *ctx = (LibraryRequestContext *)arg;

    const char *outcome_str;
    Book *book_ptr = find_book(ctx->book_title);

    if (book_ptr == NULL) {
        outcome_str = "NOT_FOUND";
    } else {
        pthread_mutex_lock(&book_ptr->lock);
        // resolve_loan reclaims a stale remote loan before we decide; 1 -> available to grant.
        if (resolve_loan(book_ptr)) {
            book_ptr->availability = LENT_OUT;
            // record the remote borrower's name so we can VERIFY this loan later
            strncpy(book_ptr->lent_to, ctx->username, sizeof(book_ptr->lent_to) - 1);
            book_ptr->lent_to[sizeof(book_ptr->lent_to) - 1] = '\0';
            book_ptr->lent_to_lib  = ctx->src_lib;
            book_ptr->really_lent  = 0;
            outcome_str = "GRANTED";
        } else {
            outcome_str = "ALREADY_LENT";
        }
        pthread_mutex_unlock(&book_ptr->lock);
    }

    char response[512];
    snprintf(response, sizeof(response), "LIB|RESPONSE|%d|%d|%s\n", lib.id, ctx->request_id, outcome_str);

    // if the peer is unreachable it will just time out; nothing we can do
    send_to_library(ctx->src_lib, response);

    free(ctx);
    return NULL;
}

void *verify_request_thread(void *arg){
    LibraryRequestContext *ctx = (LibraryRequestContext *)arg;

    const char *verdict = "NOT_HELD";
    pthread_mutex_lock(&lib.users_lock);
    User *user_ptr = find_user(ctx->username);
    pthread_mutex_unlock(&lib.users_lock);

    if (user_ptr != NULL) {
        pthread_mutex_lock(&user_ptr->lock);
        if (strcmp(user_ptr->borrowed, ctx->book_title) == 0)
            verdict = "HELD";
        pthread_mutex_unlock(&user_ptr->lock);
    }

    char message[1024];
    snprintf(message, sizeof(message), "LIB|RESPONSE|%d|%d|%s\n",
             lib.id, ctx->request_id, verdict);
    send_to_library(ctx->src_lib, message);

    free(ctx);
    return NULL;
}

void *mgmt_request_thread(void *arg){
    char *msg = (char *)arg;
    handle_mgmt_message(msg);
    free(msg);
    return NULL;
}

// ---- message handling -----------------------------------------------------

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
        // LIB|REQUEST|<src_lib>|<id>|<op>|<title>...
        char *src_s   = strtok_r(NULL, "|", &save);
        char *id_s    = strtok_r(NULL, "|", &save);
        char *op      = strtok_r(NULL, "|", &save);
        char *title   = strtok_r(NULL, "|", &save);
        if (!src_s || !id_s || !op || !title) return;
        if (strcmp(op, "BORROW") == 0) {
            // LIB|REQUEST|<src>|<id>|BORROW|<title>|<user>
            char *user = strtok_r(NULL, "|", &save);
            if (!user) return;
            LibraryRequestContext *ctx = calloc(1, sizeof(*ctx));
            if (!ctx) return;
            ctx->src_lib    = atoi(src_s);
            ctx->request_id = atoi(id_s);
            strncpy(ctx->book_title, title, sizeof(ctx->book_title) - 1);
            strncpy(ctx->username,   user,  sizeof(ctx->username)  - 1);
            pthread_t t;
            if (pthread_create(&t, NULL, borrow_request_thread, ctx) != 0) { free(ctx); return; }
            pthread_detach(t);
        }
        else if (strcmp(op, "VERIFY") == 0) {
            // LIB|REQUEST|<src>|<id>|VERIFY|<title>|<user>
            char *user = strtok_r(NULL, "|", &save);
            if (!user) return;
            LibraryRequestContext *ctx = calloc(1, sizeof(*ctx));
            if (!ctx) return;
            ctx->src_lib    = atoi(src_s);
            ctx->request_id = atoi(id_s);
            strncpy(ctx->book_title, title, sizeof(ctx->book_title) - 1);
            strncpy(ctx->username,   user,  sizeof(ctx->username)  - 1);
            pthread_t t;
            if (pthread_create(&t, NULL, verify_request_thread, ctx) != 0) { free(ctx); return; }
            pthread_detach(t);
        }
        else if (strcmp(op, "SEARCH") == 0) {
            // LIB|REQUEST|<src>|<id>|SEARCH|<field>|<value>|<resp_pipe>   (title token holds the field)
            char *value = strtok_r(NULL, "|", &save);
            char *rpipe = strtok_r(NULL, "|", &save);
            if (!value || !rpipe) return;
            SearchContext *ctx = calloc(1, sizeof(*ctx));
            if (!ctx) return;
            ctx->src_lib    = atoi(src_s);
            ctx->request_id = atoi(id_s);
            strncpy(ctx->field,     title, sizeof(ctx->field) - 1);
            strncpy(ctx->value,     value, sizeof(ctx->value) - 1);
            strncpy(ctx->resp_pipe, rpipe, sizeof(ctx->resp_pipe) - 1);
            pthread_t t;
            if (pthread_create(&t, NULL, search_request_thread, ctx) != 0) { free(ctx); return; }
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
        book_ptr->lent_to_lib = -1;
        book_ptr->really_lent = 0;
        pthread_mutex_unlock(&book_ptr->lock);
    }
}

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

// ---- listener -------------------------------------------------------------

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
