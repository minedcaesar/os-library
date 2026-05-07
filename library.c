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
} Book;
int count_lines(char file_name[]){
    char c;
    FILE *fp = fopen(file_name,"r");
    int count =0;
    for (c = getc(fp); c != EOF; c = getc(fp)) {
        if (c == '\n') {
            count++;
        }
    }
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
            catalog[current_book].author[sizeof(catalog[current_book].name) - 1] = '\0';
        }
        token = strtok(NULL, ",");
        if (token != NULL) {
            catalog[current_book].year=atoi(token);
catalog[current_book].available=1;
catalog[current_book].lent_to=NULL;
        }
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
    int id = atoi(argv[1]);
    char *catalog_file = argv[3];
    int lines = count_lines(catalog_file);
    Book * catalog = read_catalog(catalog_file,lines);
}

