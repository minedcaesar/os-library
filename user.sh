#!/bin/bash

# This script implements the functionalities/ Operations for users of a library like REGISTER/ SEARCH/ BORROW/ RETURN

# USAGE: ./user.sh <username> <library_id> <operations> [operation_args]

if [ $# -lt 3 ]; then
    echo "Invalid arguement"
    echo "USAGE: ./user.sh <username> <library_id> <operations> [operation_args]"
    exit 1
fi

username=$1
library_id=$2
operation=$(echo "$3" | tr '[:lower:]' '[:upper:]')

RESPONSE_PIPE=/tmp/user_response_$$
mkfifo $RESPONSE_PIPE

if [ $? -ne 0 ]; then
    echo "ERROR: Unable to create pipe- $RESPONSE_PIPE"
    exit 1
fi

trap "rm -f $RESPONSE_PIPE" EXIT

REQUEST_PIPE=/tmp/library_"$library_id"_request

case $operation in
    REGISTER)
    #REGISTER OPERATION

    ;;
    SEARCH)
    #SEARCH OPERATION

    ;;
    BORROW)
    #BORROW OPERATION

    ;;
    RETURN)
    #RETURN OPERATION

    ;;
    *)
    #default
    echo "ERROR: Invalid Operation"
    echo "Valid Operation: register, search, borrow, return"
    exit 1
    ;;
esac

# CHAITANYA JAISWAL