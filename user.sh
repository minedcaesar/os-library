#!/bin/bash

# This script implements the functionalities/ Operations for users of a library like REGISTER/ SEARCH/ BORROW/ RETURN

# USAGE: ./user.sh <username> <library_id> <operations> [operation_args]

if [ $# -lt 3 ]; then
    echo "Invalid arguement"
    echo "USAGE: ./user.sh <username> <library_id> <operations> [operation_args]"
    exit 1
fi

# --- Status codes: mirror of errors.h on the C side (same values, not shared). ---
readonly ERR_OK=0
readonly ERR_NO_USER=1
readonly ERR_INVALID=2
readonly ERR_NO_BOOK=3
readonly ERR_UNAVAILABLE=4
readonly ERR_NO_LOAN=5
readonly ERR_SYSTEM=6
readonly ERR_HAS_BOOK=7
readonly ERR_WRONG_BOOK=8

function check_no_pipe_char() {
    local field_name=$1
    local value=$2
    if [[ "$value" == *"|"* ]]; then
        echo "ERROR: $field_name must not contain '|'"
        exit 1
    fi
}

function check_library_pipe_status() {
    if [ ! -p "$REQUEST_PIPE" ]; then
        echo "ERROR: Library $library_id is not running"
        exit 1
    fi
}

function check_num_args_for_borrow_return() {
    local num_args=$1
    if [ $num_args -lt 4 ]; then
        echo "Invalid arguement for borrowing/ returning book"
        echo "USAGE: ./user.sh <username> <library_id> <BORROW / RETURN> <book_title>"
        exit 1
    fi
}

function check_num_args_for_search() {
    local num_args=$1
    if [ $num_args -lt 6 ]; then
        echo "Invalid arguement for searching a book"
        echo "USAGE: ./user.sh <username> <library_id> <SEARCH> <--by author/ title/ year> <AUTHOR/ TITLE/ YEAR>"
        exit 1
    fi
}

function check_flag_for_search() {
    local search_by=$1
    if [ "$search_by" != "--by" ]; then
        echo "ERROR: Expected --by flag"
        exit 1
    fi
}

function handle_search_request() {
    local field=$1
    local value=$2

    case $field in
    author|title)
        check_no_pipe_char "Search value" "$value"
        ;;
    year)
        if ! [[ "$value" =~ ^[0-9]{4}$ ]]; then
            echo "ERROR: Year must be a 4-digit number (e.g. 1995)"
            exit 1
        fi
        ;;
    *)
        echo "ERROR: Invalid search field '$field'. Use: author, title, year"
        exit 1
        ;;
    esac

    request_message="USER|SEARCH|${username}|${field}|${value}|${RESPONSE_PIPE}"
    echo "$request_message"
}


function process_request() {
    local request=$1
    check_library_pipe_status
    echo "$request" > $REQUEST_PIPE &
}

function process_response() {
    local response
    response=$(timeout 20 cat "$RESPONSE_PIPE")

    if [ -z "$response" ]; then
        echo "ERROR: No response received from the library: $library_id"
        exit 1
    fi

    local status_line body status_code first_msg
    status_line=$(printf '%s\n' "$response" | head -n1)
    body=$(printf '%s\n' "$response" | tail -n +2)
    status_code=$(printf '%s' "$status_line" | cut -d'|' -f1)
    first_msg=$(printf '%s' "$status_line" | cut -d'|' -f2-)

    [ -n "$first_msg" ] && echo "[code $status_code] $first_msg"
    [ -n "$body" ]      && printf '%s\n' "$body"

    case "$status_code" in
        "$ERR_OK")
            exit 0 ;;
        "$ERR_NO_USER"|"$ERR_INVALID"|"$ERR_NO_BOOK"|"$ERR_UNAVAILABLE"|"$ERR_NO_LOAN"|"$ERR_SYSTEM"|"$ERR_HAS_BOOK"|"$ERR_WRONG_BOOK")
            exit 1 ;;
        *)
            echo "ERROR: unknown status code '$status_code' from library $library_id"
            exit 1 ;;
    esac
}

username=$(echo "$1" | tr '[:lower:]' '[:upper:]')
library_id=$2
operation=$(echo "$3" | tr '[:lower:]' '[:upper:]')

# sanity checks:
if [[ "$username" == *"/"* ]]; then
    echo "ERROR: Username must not contain '/'"
    exit 1
fi

if ! [[ "$library_id" =~ ^[0-9]+$ ]]; then
    echo "ERROR: library_id must be a positive integer"
    exit 1
fi

check_no_pipe_char "Username" "$username"

# set up pipes:
REQUEST_PIPE=/tmp/lib_cmd_"$library_id"

RESPONSE_PIPE=/tmp/"$username"_$$
mkfifo $RESPONSE_PIPE

if [ $? -ne 0 ]; then
    echo "ERROR: Unable to create pipe- $RESPONSE_PIPE"
    exit 1
fi

trap "rm -f $RESPONSE_PIPE" EXIT

case $operation in
    REGISTER)
        #REGISTER OPERATION
        request_message="USER|REGISTER|${username}|${RESPONSE_PIPE}"
        process_request "$request_message"
        process_response
    ;;
    SEARCH)
        #SEARCH OPERATION
        check_num_args_for_search $#
        search_by=$4
        search_field=$5
        operation_arguement=$6
        check_flag_for_search "$search_by"
        request_message=$(handle_search_request "$search_field" "$operation_arguement")
        process_request "$request_message"
        process_response
    ;;
    BORROW)
        #BORROW OPERATION
        check_num_args_for_borrow_return $#
        book_title=$4
        check_no_pipe_char "Book title" "$book_title"
        request_message="USER|BORROW|${username}|${book_title}|${RESPONSE_PIPE}"
        process_request "$request_message"
        process_response
    ;;
    RETURN)
        #RETURN OPERATION
        check_num_args_for_borrow_return $#
        book_title=$4
        check_no_pipe_char "Book title" "$book_title"
        request_message="USER|RETURN|${username}|${book_title}|${RESPONSE_PIPE}"
        process_request "$request_message"
        process_response
    ;;
    *)
    #default
    echo "ERROR: Invalid Operation"
    echo "Valid Operation: register, search, borrow, return"
    exit 1
    ;;
esac

# CHAITANYA JAISWAL