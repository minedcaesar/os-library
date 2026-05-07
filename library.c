#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

typedef struct {
    char username[100];
    char borrowed[100];
} User;
typedef struct {
    char name[100];
    char author[100];
    int year;
    int available;
    User *lent_to;
    pthread_mutex_t lock; // per-book lock
} Book;

typedef struct {
    int id;
    Book *catalog;
    int num_books;
    User *users;
    int num_users;
    pthread_mutex_t users_lock; //to ensure no two users are concurrently registering with the same name
} Library;


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
        catalog[current_book].available=1;
        catalog[current_book].lent_to=NULL;
        }
        pthread_mutex_init(&catalog[current_book].lock, NULL); //initializing per book mutex
        current_book++;
    }
    fclose(fp);
    return catalog;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <library_id> <num_libraries> <catalog_file>\n", argv[0]);
        return 1;
    }

    //initializing library
    Library lib;
    lib.id = atoi(argv[1]);
    char *catalog_file = argv[3];
    lib.num_books = count_lines(catalog_file);
    lib.catalog = read_catalog(catalog_file,lib.num_books);
    if (!lib.catalog) {
        fprintf(stderr, "Failed to load catalog\n");
        return 1;
    }

    //Initializing user list
    lib.users = NULL;
    lib.num_users = 0;
    pthread_mutex_init(&lib.users_lock, NULL);


}

