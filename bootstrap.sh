#!/bin/bash

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <num_libraries> <source_csv>"
    exit 1
fi

NUM_LIBS=$1
CSV_FILE=$2

mkdir -p /tmp/    # if tmp dir from root doesn't exists

for((i=1; i<=NUM_LIBS; ++i)); do
    touch "/tmp/catalog$i.csv"
done

counter=0
while read -r line; do
    idx=$(( (counter%NUM_LIBS) + 1 ))
    echo "$line" >> "/tmp/catalog$idx.csv"
    ((counter++))
done < <(tail -n +2 "$CSV_FILE")     # this obbligate to run tail into the main so that 

echo "Distributed $counter books into $NUM_LIBS catalogs."

for((i=1; i<=NUM_LIBS; ++i)); do
    echo "Launching Library $i..."
    ./library "$i" "$NUM_LIBS" "/tmp/catalog$i.csv" &
done

wait  #this is done to wait for the background lib processes to be waited

echo "System bootstrapped with $NUM_LIBS libraries."
