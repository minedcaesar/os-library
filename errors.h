#ifndef ERRORS_H
#define ERRORS_H

/*
 * Status / error codes for every user-facing operation.
 *
 * These are the single source of truth on the C side; `user.sh` mirrors the same
 * values as shell variables (see the "status codes" block near the top of user.sh),
 * so the two stay consistent without sharing a file — exactly as the spec allows.
 *
 * The library answers each user request with a "<code>|<message>" line (see
 * send_status() in protocol.c). user.sh reads field 1, maps code 0 to a successful
 * exit and any non-zero code to a failure exit, and relays field 2 to the user.
 */

#define ERR_OK           0   /* success: registered / lent / returned / search results */
#define ERR_NO_USER      1   /* no such user (not registered with this library)         */
#define ERR_INVALID      2   /* user already registered, or invalid search field        */
#define ERR_NO_BOOK      3   /* no such book: found in no catalog (borrow) or here (return) */
#define ERR_UNAVAILABLE  4   /* book is already lent out                                */
#define ERR_NO_LOAN      5   /* user has no book to return                              */
#define ERR_SYSTEM       6   /* system busy / allocation failure — retry                */
#define ERR_HAS_BOOK     7   /* user already holds a book (must return first)           */
#define ERR_WRONG_BOOK   8   /* user does not hold that particular book                 */

#endif /* ERRORS_H */
