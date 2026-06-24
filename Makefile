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
	@echo "Build complete. Start a scenario with the bootstrapping script, e.g.:"
	@echo "    ./bootstrap.sh <num_libraries> <source_csv>"
	@echo "    ./bootstrap.sh 3 books.csv"
