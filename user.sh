#!/bin/bash

# This script implements the functionalities/ Operations for users of a library like REGISTER/ SEARCH/ BORROW/ RETURN

# USAGE: ./user.sh <username> <library_id> <operations> [operation_args]

if [ $# -lt 3 ]; then
    echo "Invalid arguement"
    echo "USAGE: ./user.sh <username> <library_id> <operations> [operation_args]"
    exit 1
fi

username=$(echo "$1" | tr '[:lower:]' '[:upper:]')
library_id=$2
operation=$(echo "$3" | tr '[:lower:]' '[:upper:]')

RESPONSE_PIPE=/tmp/"$username"_$$
mkfifo $RESPONSE_PIPE

if [ $? -ne 0 ]; then
    echo "ERROR: Unable to create pipe- $RESPONSE_PIPE"
    exit 1
fi

trap "rm -f $RESPONSE_PIPE" EXIT

REQUEST_PIPE=/tmp/library_"$library_id"

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
        author|title|year)
            request_message="USER|SEARCH|${username}|${field}|${value}|${RESPONSE_PIPE}"
            echo "$request_message"
        ;;
        *)
            echo "ERROR: Invalid search field '$field'. Use: author, title, year"
            exit 1
        ;;
    esac
}

function process_request() {
    local request=$1
    check_library_pipe_status
    echo "$request" > $REQUEST_PIPE &
}

function process_response() {
    response=$(timeout 10 head -1 $RESPONSE_PIPE)

    if [ -z "$response" ]; then
        echo "ERROR: No response received from the library: $library_id"
        exit 1
    fi

    status_code=$(echo "$response" | cut -d'|' -f1)
    message=$(echo "$response" | cut -d'|' -f2-)

    case $status_code in
        0)
        echo "$message"
        exit 0
        ;;
        *)
        echo "$message"
        exit 1
        ;;
    esac
}

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
        request_message="USER|BORROW|${username}|${book_title}|${RESPONSE_PIPE}"
        process_request "$request_message"
        process_response
    ;;
    RETURN)
        #RETURN OPERATION
        check_num_args_for_borrow_return $#
        book_title=$4
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
