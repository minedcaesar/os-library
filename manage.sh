#!/bin/bash

OPERATION=$1
LIBS_FOUND=$(ls /tmp/lib_cmd_* 2>/dev/null | grep -o '[0-9]\+')

case $OPERATION in
    "status")
        echo "Querying global library node statuses..."
        rm -f library_status_*.txt
        
        # Broadcast SIGUSR1 to all running libraries
        pkill -SIGUSR1 library
        sleep 0.5
        
        if ls library_status_*.txt 1> /dev/null 2>&1; then
            cat library_status_*.txt
        else
            echo "No operational libraries detected."
        fi
        ;;
        
    "list_books")
        echo "Fetching real-time book catalogs..."
        if [ -z "$LIBS_FOUND" ]; then    # -z means is empty
            echo "No running libraries found."
            exit 0
        fi

        RESP_PIPE="/tmp/mgmt_books_$$"
        mkfifo "$RESP_PIPE"

        for id in $LIBS_FOUND; do
            if [ -p "/tmp/lib_cmd_$id" ]; then   # -p is named pipe, FIFO
                # Ask each library process directly via its command FIFO
                echo "MGMT|LIST_BOOKS|$RESP_PIPE" > "/tmp/lib_cmd_$id" &

                if ! timeout 2 cat "$RESP_PIPE" 2>/dev/null; then     # timeout to don't hang forever
                    echo "[!] Library node $id timed out or failed to respond."
                fi
            fi
        done

        rm -f "$RESP_PIPE"
        ;;
    
    #almost the same as above
    "list_users")
        echo "Querying multi-threaded client indexes..."
        if [ -z "$LIBS_FOUND" ]; then
            echo "No operational libraries detected."
            exit 0
        fi

        RESP_PIPE="/tmp/mgmt_resp_$$"
        mkfifo "$RESP_PIPE"

        for id in $LIBS_FOUND; do
            if [ -p "/tmp/lib_cmd_$id" ]; then
                echo "MGMT|LIST_USERS|$RESP_PIPE" > "/tmp/lib_cmd_$id" &
                
                if ! timeout 2 cat "$RESP_PIPE" 2>/dev/null; then
                    echo "[!] Library node $id timed out or failed to respond."
                fi
            fi
        done

        rm -f "$RESP_PIPE"
        ;;

    "stop")
        echo "Gracefully spinning down instance clusters..."
        pkill -SIGTERM library
        sleep 0.5
        
        echo "Cleaning persistent building blocks via Makefile..."
        # Trigger clean rules in your local workspace
        make clean 2>/dev/null || rm -f /tmp/lib_cmd_*
        echo "System offline."
        ;;

    *)
        echo "Usage: $0 {status|list_books|list_users|stop}"
        exit 1
        ;;
esac