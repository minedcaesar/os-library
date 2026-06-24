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
ifeq ($(strip $(ARGS)),)
	@echo "Compilation succeeded. No scenario started: pass ARGS to bootstrap one, e.g.:"
	@echo "    make run ARGS=\"3 books.csv\""
else
	./bootstrap.sh $(ARGS)
endif
