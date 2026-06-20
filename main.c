#include "library.h"

// The single global library instance for this process.
Library lib;

int main(int argc, char *argv[]) {
    if (argc != 4)
    {
        fprintf(stderr, "Usage: %s <library_id> <num_libraries> <catalog_file>\n", argv[0]);
        return 1;
    }

    srand(time(NULL));
    for (int i = 0; i < MAX_PENDING; i++) {
        pthread_mutex_init(&lib.pending[i].lock, NULL);
        pthread_cond_init(&lib.pending[i].cond, NULL);
        atomic_store(&lib.pending[i].in_use, 0);
    }

    // 1. Block Management Signals Early
    // Threads inherit the signal mask of their creator. By blocking SIGUSR1 and SIGTERM
    // here, we guarantee that no random background worker thread gets interrupted by them.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
        perror("pthread_sigmask failed");
        return 1;
    }

    // 2. Ignore SIGPIPE Safely
    // Prevents the library process from crashing if a client closes their response pipe early
    signal(SIGPIPE, SIG_IGN);

    // 3. Initialize Library Metadata & Catalog
    lib.id = atoi(argv[1]);
    lib.num_total_libraries = atoi(argv[2]);
    lib.next_id = 0;
    char *catalog_file = argv[3];

    lib.num_books = count_lines(catalog_file);
    if (lib.num_books == -1) {
        perror("Cannot read CSV catalog file");
        return -1;
    }

    lib.catalog = read_catalog(catalog_file, lib.num_books);
    if (!lib.catalog)
    {
        fprintf(stderr, "Failed to load catalog into memory\n");
        return 1;
    }

    // 4. Initialize User Registry
    lib.capacity = 50;
    lib.users = malloc(lib.capacity * sizeof(User *));
    if (!lib.users) {
        perror("Failed to allocate initial users array");
        free(lib.catalog);
        return 1;
    }
    lib.num_users = 0;
    pthread_mutex_init(&lib.users_lock, NULL);

    // 5. Setup the Incoming Command FIFO Command Channel
    snprintf(lib.pipe_path, sizeof(lib.pipe_path), "/tmp/lib_cmd_%d", lib.id);

    // Clean up an old pipe node if it was left over from a crash
    unlink(lib.pipe_path);
    if (mkfifo(lib.pipe_path, 0666) < 0) {
        perror("mkfifo failed for incoming command pipeline");
        free(lib.users);
        free(lib.catalog);
        return 1;
    }

    // Open in Read/Write mode so our listener thread doesn't read a constant 0 (EOF)
    // when individual client shell scripts disconnect.
    lib.pipe_fd = open(lib.pipe_path, O_RDWR);
    if (lib.pipe_fd < 0) {
        perror("Failed to open command FIFO");
        unlink(lib.pipe_path);
        free(lib.users);
        free(lib.catalog);
        return 1;
    }

    // 6. Spawn the Background Listener Thread
    // This thread handles incoming commands (BORROW, MGMT|LIST_BOOKS, MGMT|LIST_USERS)
    if (pthread_create(&lib.listener_thread, NULL, listener_thread, NULL) != 0) {
        perror("pthread_create failed for listener_thread");
        close(lib.pipe_fd);
        unlink(lib.pipe_path);
        free(lib.users);
        free(lib.catalog);
        return 1;
    }

    // 7. Synchronous Management Loop (Main Thread)
    // Instead of jumping into thread logs, the main thread sleeps right here until manage.sh
    // signals it. Because it wakes up synchronously, it is 100% safe to do file I/O and locks.
    int sig;
    while (1) {
        // Blocks until SIGUSR1 or SIGTERM arrives for this process
        sigwait(&set, &sig);

        if (sig == SIGUSR1) {
            // FUNCTION 1: Dump structural node diagnostics
            char filename[256];
            snprintf(filename, sizeof(filename), "library_status_%d.txt", lib.id);

            FILE *fp = fopen(filename, "w");
            if (fp) {
                fprintf(fp, "=== Library Node [%d] ===\n", lib.id);
                fprintf(fp, "Status: Operational / Running\n");
                fprintf(fp, "Active Clients Registered: %d\n", lib.num_users);
                fprintf(fp, "Catalog Volume Size: %d\n\n", lib.num_books);
                fclose(fp);
            }
        }
        else if (sig == SIGTERM) {
            // FUNCTION 4: Clean up IPC system resources and shut down
            printf("[Library %d] Received shutdown signal. Cleaning up...\n", lib.id);

            // Critical step: Dropping the file system link address off the machine layout
            close(lib.pipe_fd);
            unlink(lib.pipe_path);

            // Clean up allocated memory space
            pthread_mutex_lock(&lib.users_lock);
            for(int i = 0; i < lib.num_users; i++) {
                pthread_mutex_destroy(&lib.users[i]->lock);
                free(lib.users[i]);
            }
            free(lib.users);
            pthread_mutex_unlock(&lib.users_lock);
            pthread_mutex_destroy(&lib.users_lock);

            for(int i = 0; i < lib.num_books; i++) {
                pthread_mutex_destroy(&lib.catalog[i].lock);
            }
            free(lib.catalog);

            printf("[Library %d] Resources freed. System offline.\n", lib.id);
            exit(0);
        }
    }

    return 0; // Never reached logically
}
