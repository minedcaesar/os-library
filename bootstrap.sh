#!/bin/bash

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <num_libraries> <source_csv>"
    exit 1
fi

NUM_LIBS=$1
CSV_FILE=$2

mkdir -p /tmp/    # if tmp dir from root doesn't exists

counter=0
tail -n +2 "$CSV_FILE" | while read -r line; do    #the tail -n +2 starts the read from the second line of the FILE
    idx=$(( (counter%LIBS_NUM) +1 ))

    echo "$line" >> "/tmp/catalog$idx.csv"

    ((counter++))
done

echo "Distributed $counter books into $NUM_LIBS catalogs."

for((i=1; i<=NUM_LIBS; ++i)); do
    echo "Launching Library $i..."
    ./library "$i" "$NUM_LIBS" "/tmp/catalog$i.csv" &
done

wait  #this is done to wait for the background lib processes to be waited

echo "System bootstrapped with $NUM_LIBS libraries."
