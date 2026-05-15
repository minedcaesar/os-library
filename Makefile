CC = gcc
CFLAGS = -Wall -Wextra -g

TARGET = library

build: $(TARGET)
	chmod +x bootstrap.sh manage.sh user.sh

$(TARGET): library.c
	$(CC) $(CFLAGS) -o $(TARGET) library.c

clean:
	rm -f $(TARGET) catalog*.csv
	#also we have to remove the IPC

run: build
	./bootstrap.sh $(ARGS)
