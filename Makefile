CC = gcc
CFLAGS = -Wall -Wextra -g -pthread

TARGET = library
OBJS = main.o catalog.o operations.o protocol.o

build: $(TARGET)
	chmod +x bootstrap.sh manage.sh user.sh		

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c library.h library_types.h errors.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	@./manage.sh stop
	rm -f *.o $(TARGET)

run: build
	./bootstrap.sh $(ARGS)
