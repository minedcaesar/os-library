#include "library.h"

// adds user to lib.users
void register_user(char *username, int fd) {
    pthread_mutex_lock(&lib.users_lock);

    for (int i = 0; i < lib.num_users; i++)
    {
        if (strcmp(lib.users[i]->username, username) == 0)
        {
            pthread_mutex_unlock(&lib.users_lock);
            send_status(fd, ERR_INVALID, "User already registered");
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
            send_status(fd, ERR_SYSTEM, "Failed to reallocate user list, please retry");
            return;
        }
        lib.users = tmp;
        lib.capacity = newcap;
    }
    User *u = malloc(sizeof(User));
    if (!u)
    {
        pthread_mutex_unlock(&lib.users_lock);
        send_status(fd, ERR_SYSTEM, "Malloc failed");
        return;
    }
    strncpy(u->username, username, sizeof(u->username) - 1);
    u->username[sizeof(u->username) - 1] = '\0';
    u->borrowed[0] = '\0';
    pthread_mutex_init(&u->lock, NULL);
    u->borrowed_from_lib = -1;
    lib.users[lib.num_users++] = u;
    pthread_mutex_unlock(&lib.users_lock);
    send_status(fd, ERR_OK, "User registered");
}

// Caller holds book->lock; book is being considered for lending. If it is an *unconfirmed*
// remote loan (lent_to_lib != -1, really_lent == 0), VERIFY with the borrowing library —
// dropping book->lock for the round-trip — and reclaim the book if the borrower no longer
// holds it. The lock is re-acquired before returning.
// Returns 1 if the book is now AVAILABLE (caller may lend it), 0 if genuinely unavailable.
int resolve_loan(Book *book) {
    if (book->availability == AVAILABLE)
        return 1;
    if (book->lent_to_lib == -1)
        return 0;   // a local loan -> genuinely unavailable

    // A HELD confirmation is only a *lease*: trust it (skip the round-trip) while it is valid,
    // but once it lapses we must re-verify, so a lost LIB|RETURN can still be reconciled.
    time_t now = time(NULL);
    if (book->really_lent == 1 && now < book->verified_until)
        return 0;   // confirmed remote loan, lease still valid

    // Unconfirmed or lease-expired remote loan: snapshot what the round-trip needs, then verify
    // without holding the lock.
    int  target = book->lent_to_lib;
    char title[100], borrower[100];
    strncpy(title,    book->name,    sizeof(title) - 1);    title[sizeof(title) - 1]       = '\0';
    strncpy(borrower, book->lent_to, sizeof(borrower) - 1); borrower[sizeof(borrower) - 1] = '\0';
    pthread_mutex_unlock(&book->lock);

    enum Outcome o; int who = -1;
    int rc = lib_request(target, &o, &who, "VERIFY|%s|%s", title, borrower);

    pthread_mutex_lock(&book->lock);
    // Apply the verdict only if it is still the same loan we set out to verify (the book may have
    // been returned or re-lent during the round-trip). Note: we do NOT require really_lent == 0
    // here, because re-verifying an expired lease starts from really_lent == 1.
    if (book->availability == LENT_OUT && book->lent_to_lib == target) {
        if (rc == 0 && o == NOT_HELD) {
            book->availability = AVAILABLE;   // borrower no longer holds it -> reclaim
            book->lent_to_lib  = -1;
            book->really_lent  = 0;
            book->lent_to[0]   = '\0';
        } else if (o == HELD) {
            book->really_lent    = 1;                          // confirmed...
            book->verified_until = time(NULL) + VERIFY_CACHE_TTL_SEC;  // ...but only for a bounded lease
        }
    }

    return book->availability == AVAILABLE;
}

// core operations
void borrow_book(char *username, char *book_title, int fd) {

    // Must lock users list to safely read and prevent realloc invalidation
    pthread_mutex_lock(&lib.users_lock);
    User *user_ptr = find_user(username);
    pthread_mutex_unlock(&lib.users_lock);

    if (user_ptr == NULL)
    {
        send_status(fd, ERR_NOT_FOUND, "No such User.");
        return;
    }
    pthread_mutex_lock(&user_ptr->lock);
    if (user_ptr->borrowed[0] != '\0')
    {
        send_status(fd, ERR_HAS_BOOK, "User has a book");
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
        if (lib_request(BROADCAST_ALL, &outcome, &lib_responder, "BORROW|%s|%s", book_title, username) < 0) {
            send_status(fd, ERR_SYSTEM, "System busy, try again later");
            pthread_mutex_unlock(&user_ptr->lock);
            return;
        }
        if (outcome == LENT) {
            strncpy(user_ptr->borrowed, book_title, sizeof(user_ptr->borrowed) - 1);
            user_ptr->borrowed[sizeof(user_ptr->borrowed) - 1] = '\0';
            user_ptr->borrowed_from_lib = lib_responder;
            send_status(fd, ERR_OK, "Book has successfully been lent");
        }
        else if (outcome == ALREADY_LENT) {
            send_status(fd, ERR_UNAVAILABLE, "Book was already lent to another user");
        }
        else { // PENDING — timed out, or nobody had the book
            send_status(fd, ERR_NOT_FOUND, "No such book.");
        }
        pthread_mutex_unlock(&user_ptr->lock);

        return;
    }
    // CASE B — Book is in *this* library's catalog. Local borrow.
    pthread_mutex_lock(&(book_ptr->lock));

    // resolve_loan verifies/reclaims a stale remote loan if needed; 1 -> we can lend it locally.
    if (resolve_loan(book_ptr)) {
        book_ptr->availability = LENT_OUT;
        book_ptr->lent_to_lib  = -1;        // it's a local loan now
        book_ptr->really_lent  = 0;
        strncpy(book_ptr->lent_to, username, sizeof(book_ptr->lent_to)-1);
        book_ptr->lent_to[sizeof(book_ptr->lent_to)-1]='\0';
        strncpy(user_ptr->borrowed, book_title, sizeof(user_ptr->borrowed)-1);
        user_ptr->borrowed[sizeof(user_ptr->borrowed)-1]='\0';
        send_status(fd, ERR_OK, "Success");
    }
    else {
        send_status(fd, ERR_UNAVAILABLE, "Book not available");
    }
    pthread_mutex_unlock(&book_ptr->lock);
    pthread_mutex_unlock(&user_ptr->lock);

    return;
}

void return_book(char *username, char *book_title, int fd) {

    pthread_mutex_lock(&lib.users_lock);
    User *user_ptr = find_user(username);
    pthread_mutex_unlock(&lib.users_lock);

    if (user_ptr == NULL)
    {
        send_status(fd, ERR_NOT_FOUND, "No such user");
        return;
    }
    // check if they have no book
    pthread_mutex_lock(&user_ptr->lock);
    if ((user_ptr->borrowed)[0] == '\0')
    {
        send_status(fd, ERR_NO_LOAN, "User doesnt have a book");
        pthread_mutex_unlock(&user_ptr->lock);
        return;
    }
    // check if they are trying to return the right book
    if (strcmp(user_ptr->borrowed, book_title))
    {
        send_status(fd, ERR_WRONG_BOOK, "User doesnt have that book");
        pthread_mutex_unlock(&user_ptr->lock);
        return;
    }

    if(user_ptr->borrowed_from_lib!=-1){
        char buffer[1024];
        snprintf(buffer, sizeof(buffer), "LIB|RETURN|%d|%s\n", lib.id, book_title);
        if(send_to_library(user_ptr->borrowed_from_lib, buffer) < 0){
            send_status(fd, ERR_SYSTEM, "System busy, try later");
            pthread_mutex_unlock(&user_ptr->lock);
            return;
        }
        user_ptr->borrowed[0] = '\0';
        user_ptr->borrowed_from_lib=-1;
        send_status(fd, ERR_OK, "Successfully returned the book");
        pthread_mutex_unlock(&user_ptr->lock);
        return;
    }

    Book *book_ptr = find_book(book_title);
    if (book_ptr == NULL)
    {
        send_status(fd, ERR_NO_BOOK, "No such book");
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
        book_ptr->lent_to_lib=-1;
        book_ptr->really_lent=0;
        pthread_mutex_unlock(&book_ptr->lock);
        pthread_mutex_unlock(&user_ptr->lock);
        send_status(fd, ERR_OK, "Success");
    }
}

void search_book(char* username,char *field, char *value,int fd, char* response_pipe) {


    pthread_mutex_lock(&lib.users_lock);
    int known = (find_user(username) != NULL);
    pthread_mutex_unlock(&lib.users_lock);
    if (!known) {
        send_status(fd, ERR_NOT_FOUND, "No such user.");
        return;
    }

    int by_author = (strcmp(field, "author") == 0);
    int by_title  = (strcmp(field, "title")  == 0);
    int by_year   = (strcmp(field, "year")   == 0);

    if (!by_author && !by_title && !by_year) {
        char err[160];
        snprintf(err, sizeof(err), "Invalid search field '%s'\n", field);
        send_status(fd, ERR_INVALID, err);
        return;
    }

    sleep(rand()%5 +1);

    // Status line first so user.sh always reads a code, then our own local matches.
    send_status(fd, ERR_OK, "Search results:\n");
    for (int i = 0; i < lib.num_books; i++) {
        if (matches(&lib.catalog[i], by_author, by_title, by_year, value)) {
            char line[512];
            snprintf(line, sizeof(line), "%s by %s (%d)\n", lib.catalog[i].name, lib.catalog[i].author, lib.catalog[i].year);
            send_message(line, fd);
        }
    }

    // Ask every other library to stream its matches *directly* to the user's pipe. Only the
    // small query rides the library FIFO; the (possibly large) results never touch it. We then
    // block until every peer sends a DONE ack (counted by the pending quorum) or the TTL fires,
    // so we keep the user's pipe open until all writers have finished.
    if (lib.num_total_libraries > 1) {
        enum Outcome o; int who = -1;
        lib_request(BROADCAST_ALL, &o, &who, "SEARCH|%s|%s|%s", field, value, response_pipe);
        // outcome ignored we only needed the DONE quorum, not a verdict.
    }
}
