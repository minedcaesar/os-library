CC = gcc
CFLAGS = -Wall -Wextra -g

TARGET = library

build: $(TARGET)
	chmod +x bootstrap.sh manage.sh user.sh

$(TARGET): library.c
	$(CC) $(CFLAGS) -o $(TARGET) library.c

clean:
	@./manage.sh stop
# the @ is used to not print the command ./manage.sh stop

run: build
	./bootstrap.sh $(ARGS)
