#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>

// LOCKS MUST ALWAYS FOLLOW THIS ORDER TO PREVENT DEADLOCKS: USER, THEN BOOK
enum Availability
{
    available,
    lent
};

typedef struct {
    char username[100];
    char borrowed[100];
} User;

typedef struct {
    char name[100];
    char author[100];
    int year;
    enum Availability available;
    char lent_to[100];
    pthread_mutex_t lock; // per-book lock
} Book;

typedef struct {
    int id;
    Book *catalog;
    int num_books;
    User *users;
    int num_users;
    int capacity;
    pthread_mutex_t users_lock; //to ensure no two users are concurrently registering with the same name
    int request_fd;
    int response_fd;

} Library;


Library lib;

int count_lines(char file_name[]){
    char c;
    FILE *fp = fopen(file_name,"r");
    int count =0;
    for (c = getc(fp); c != EOF; c = getc(fp)) {
        if (c == '\n') {
            count++;
        }
    }
    fclose(fp);
    return count;
}

Book* read_catalog(char *catalog_file,int lines){
    int MAX_FIELD_LENGTH=1024;
    FILE *fp = fopen(catalog_file,"r");
    if (!fp) return NULL;
    Book* catalog = (Book*)malloc(lines*sizeof(Book));
    char row[MAX_FIELD_LENGTH];
    int current_book=0;
    while (fgets(row, sizeof(row), fp) && current_book < lines) {
        
        row[strcspn(row, "\r\n")] = 0;
        
        if (strlen(row) == 0) continue;

        char *token = strtok(row, ",");
        if (token != NULL) {
            strncpy(catalog[current_book].name, token, sizeof(catalog[current_book].name) - 1);
            catalog[current_book].name[sizeof(catalog[current_book].name) - 1] = '\0';
        }
        token = strtok(NULL, ",");
        if (token != NULL) {
            strncpy(catalog[current_book].author, token, sizeof(catalog[current_book].name) - 1);
            catalog[current_book].author[sizeof(catalog[current_book].author) - 1] = '\0';
        }
        token = strtok(NULL, ",");
        if (token != NULL) {
            catalog[current_book].year=atoi(token);
        catalog[current_book].available=available;
        catalog[current_book].lent_to=NULL;
        }
        pthread_mutex_init(&catalog[current_book].lock, NULL); //initializing per book mutex
        current_book++;
    }
    fclose(fp);
    return catalog;
}

void register_user(char * username){
    pthread_mutex_lock(&lib.users_lock);

    // Loop up to num_users, not capacity
    for (int i = 0; i < lib.num_users; i++){
        if(strcmp(lib.users[i].username, username) == 0){
            perror("Username already taken");
            pthread_mutex_unlock(&lib.users_lock); // Unlock before returning
            // TODO: send error to user.sh
            return;
        }
    }

    if(lib.num_users == lib.capacity){
        lib.capacity *= 2;
        User *temp = realloc(lib.users, lib.capacity * sizeof(User));
        if (temp == NULL){
            perror("Failed to reallocate");
            pthread_mutex_unlock(&lib.users_lock);
            // TODO: send error to user.sh
            return;
        }
        lib.users = temp;
    }

    strncpy(lib.users[lib.num_users].username, username, 99);
    lib.users[lib.num_users].username[99] = '\0';
    lib.users[lib.num_users].borrowed[0] = '\0'; 
    lib.num_users++;
    
    pthread_mutex_unlock(&lib.users_lock);
    // TODO: send success to user.sh
}


void borrow_book(char* username, char* book_title){
    Book *book_ptr = NULL;
    for (int i = 0; i < lib.num_books; i++){
        if (strcmp(lib.catalog[i].name, book_title) == 0){
            book_ptr = &lib.catalog[i];
            break;
        }
    }
    
    if(book_ptr == NULL){
        // TODO: ask other libraries if they have the book
    }

    // Must lock users list to safely read and prevent realloc invalidation
    pthread_mutex_lock(&lib.users_lock);
    User *user_ptr = NULL;
    for (int i = 0; i < lib.num_users; i++){
        if (strcmp(lib.users[i].username, username) == 0){
            user_ptr = &lib.users[i];
            break;
        }
    }
    
    if (user_ptr == NULL){
        perror("No such user");
        pthread_mutex_unlock(&lib.users_lock);
        // TODO: send error to user.sh
        return;
    }

    pthread_mutex_lock(&book_ptr->lock);
    

    if(book_ptr->available == available){
        book_ptr->available = lent; 
        strncpy(user_ptr->borrowed, book_title, sizeof(user_ptr->borrowed) - 1);
        user_ptr->borrowed[sizeof(user_ptr->borrowed) - 1] = '\0';
        strncpy(book_ptr->lent_to, username, sizeof(book_ptr->lent_to) - 1);
        book_ptr->lent_to[sizeof(book_ptr->lent_to)-1]='\0';

    } else {
        perror("Book is not available");
        // TODO: handle unavailable logic
    }
    
    pthread_mutex_unlock(&book_ptr->lock);
    pthread_mutex_unlock(&lib.users_lock);
}

void return_book(char * username,char* book_title)
{
    pthread_mutex_lock(&lib.users_lock);
    User *user_ptr = NULL;
    for (int i = 0; i < lib.num_users; i++){
        if (strcmp(lib.users[i].username, username) == 0){
            user_ptr = &lib.users[i];
            break;
        }
    }
    if(user_ptr==NULL){
        perror("No such user");
        // TODO: send error to user.sh
        pthread_mutex_unlock(&lib.users_lock);
        return;
    }
    Book *book_ptr = NULL;
    for (int i = 0; i < lib.num_books; i++){
        if (strcmp(lib.catalog[i].name, book_title) == 0){
            book_ptr = &lib.catalog[i];
            break;
        }
    }
    
    if(book_ptr == NULL){
        // TODO: ask other libraries if they have the book
    }
    else{
        phtread_mutex_lock(book_ptr->lock);
        book_ptr->available = available;
        strncpy(book_ptr->lent_to, "", sizeof(book_ptr->lent_to));
        strncpy(user_ptr->borrowed, "", sizeof(user_ptr->borrowed));
        pthread_mutex_unlock(&book_ptr->lock);
    }
    pthread_mutex_lock(&lib.users_lock);
}

void setup_user_ipc(){
    char request_fifo[256];
    char response_fifo[256];
    
    sprintf(request_fifo, "/tmp/library_%d_request", lib->id);
    sprintf(response_fifo, "/tmp/library_%d_response", lib->id);

    unlink(request_fifo);
    unlink(response_fifo);

    if (mkfifo(request_fifo, 0666) == -1) {
        perror("mkfifo request");
        exit(1);
    }
    if (mkfifo(response_fifo, 0666) == -1) {
        perror("mkfifo response");
        exit(1);
    }
    
}


int main(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <library_id> <num_libraries> <catalog_file>\n", argv[0]);
        return 1;
    }

    //initializing library
    lib.id = atoi(argv[1]);
    char *catalog_file = argv[3];
    lib.num_books = count_lines(catalog_file);
    lib.catalog = read_catalog(catalog_file,lib.num_books);
    if (!lib.catalog) {
        fprintf(stderr, "Failed to load catalog\n");
        return 1;
    }

    //Initializing user list
    lib.capacity = 50;
    lib.users = malloc(lib.capacity * sizeof(User));
    lib.num_users = 0;
    pthread_mutex_init(&lib.users_lock, NULL);
 


}

