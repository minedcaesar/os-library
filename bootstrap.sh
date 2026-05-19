#!/bin/bash

LIBS_NUM=$1
FILE=$2

if [[ -z "$NUM_LIBS" || ! -f "$SOURCE_CSV" ]]; then
    echo "Usage: $0 <num_libraries> <source_csv>"
    exit 1
fi

header=$(head -n 1 "$FILE")

for ((i=1; i<= LIBS_NUM; ++i)); do
    echo "$header" > "/tmp/catalog$i.csv"
done

counter = 0
tail -n +2 "$FILE" | while read -r line; do    #the tail -n +2 starts the read from the second line of the FILE
    idx = $(( (counter%LIBS_NUM) +1 ))

    echo "$line" > "/tmp/catalog$idx.csv"

    ((counter++))
done

echo "Distributed $counter books into $LIBS_NUM catalogs."

for((i=1; i<=LIBS_NUM; ++i)); do
    echo "Launching Library $i..."
    ./library "$i" "$LIBS_NUM" "/tmp/catalog$i.csv" &
done

wait  #this is done to wait for the background lib processes to be waited

echo "System bootstrapped with $LIBS_NUM libraries."
