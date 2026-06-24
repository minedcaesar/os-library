#ifndef LIBRARY_H
#define LIBRARY_H

#define _POSIX_C_SOURCE 200809L

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>

#include "library_types.h"
#include "errors.h"

/* The single global library instance; defined in main.c. */
/* (library_types.h already declares `extern Library lib;`) */

/* ---- catalog.c : catalog loading and lookups ---- */
int   count_lines(char file_name[]);
Book *read_catalog(char *catalog_file, int lines);
Book *find_book(const char *title);
User *find_user(const char *username);          /* caller must hold users_lock */
int   matches(const Book *b, int by_author, int by_title, int by_year, const char *value);

/* ---- operations.c : user-facing operations ---- */
void  register_user(char *username, int fd);
int   resolve_loan(Book *book);                 /* caller holds book->lock */
void  borrow_book(char *username, char *book_title, int fd);
void  return_book(char *username, char *book_title, int fd);
void  search_book(char *username, char *field, char *value, int fd, char *response_pipe);

/* ---- protocol.c : IPC, listener, request/response handling, worker threads ---- */
void  send_message(char *message, int fd);
void  send_status(int fd, int code, const char *message);  /* emits "<code>|<message>" */
int   send_to_library(int lib_id, const char *msg);
int   request_id(void);
int   lib_request(int target, enum Outcome *outcome, int *responder, const char *fmt, ...)
          __attribute__((format(printf, 4, 5)));
void  handle_pending(int id, char *response, int lib_id);
void  process_message(char *buffer);
void *listener_thread(void *arg);
void  handle_user_message(char *message);
void  handle_library_request(char *message);
void  handle_mgmt_message(char *message);
void *mgmt_request_thread(void *arg);
void *user_request_thread(void *arg);
void *borrow_request_thread(void *arg);
void *verify_request_thread(void *arg);
void *search_request_thread(void *arg);

#endif /* LIBRARY_H */
