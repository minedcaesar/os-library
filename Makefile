CC = gcc
CFLAGS = -Wall -Wextra -g

TARGET = library

build: $(TARGET)
	chmod +x bootstrap.sh manage.sh user.sh

$(TARGET): library.c
	$(CC) $(CFLAGS) -o $(TARGET) library.c

clean:
	rm -f ./$(TARGET) /tmp/catalog*.csv /tmp/lib_cmd*

run: build
	./bootstrap.sh $(ARGS)
