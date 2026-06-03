#!/bin/bash

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <num_libraries> <source_csv>"
    exit 1
fi

NUM_LIBS=$1
CSV_FILE=$2


for((i=1; i<=NUM_LIBS; ++i)); do
    touch "/tmp/catalog$i.csv"
done

counter=0
while read -r line; do
    idx=$(( (counter%NUM_LIBS) + 1 ))
    echo "$line" >> "/tmp/catalog$idx.csv"
    ((counter++))
done < <(tail -n +2 "$CSV_FILE")     # <() obbligate to treat the output as a virtual file and then load it into the while

echo "Distributed $counter books into $NUM_LIBS catalogs."

for((i=1; i<=NUM_LIBS; ++i)); do
    echo "Launching Library $i..."
    ./library "$i" "$NUM_LIBS" "/tmp/catalog$i.csv" &
done


echo "System bootstrapped with $NUM_LIBS libraries."
