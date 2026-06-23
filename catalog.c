#include "library.h"

// Counts the lines in a file (used to size the catalog array before reading it).
int count_lines(char file_name[]) {
    FILE *fp = fopen(file_name, "r");
    if (!fp) {
        perror(file_name);
        return -1;
    }
    int count = 0;
    int c, last = '\n';
    for (c = getc(fp); c != EOF; c = getc(fp))
    {
        if (c == '\n')
        {
            count++;
        }
        last = c;
    }
    if(last!='\n' && last != EOF)
        count++;
    fclose(fp);
    return count;
}

// Reads the next CSV field from *cursor into out[outsz], honouring double-quoted fields
// (commas inside quotes are literal; "" is an escaped quote). Advances *cursor past the
// field's trailing comma. Returns out, or NULL when there are no more fields on the line.
static char *next_csv_field(char **cursor, char *out, size_t outsz) {
    char *p = *cursor;
    size_t o = 0;
    if (*p == '\0') { out[0] = '\0'; return NULL; }

    if (*p == '"') {
        p++;                                       // opening quote
        while (*p) {
            if (*p == '"' && *(p + 1) == '"') {    // escaped quote  ""
                if (o + 1 < outsz) out[o++] = '"';
                p += 2;
            } else if (*p == '"') {                // closing quote
                p++;
                break;
            } else {
                if (o + 1 < outsz) out[o++] = *p;
                p++;
            }
        }
        while (*p && *p != ',') p++;               // skip to the separator
    } else {
        while (*p && *p != ',') {
            if (o + 1 < outsz) out[o++] = *p;
            p++;
        }
    }
    out[o] = '\0';
    if (*p == ',') p++;                            // consume the separator
    *cursor = p;
    return out;
}

//reads from a catalog txt file and returns a Book* catalog.
Book *read_catalog(char *catalog_file, int lines) {
    int MAX_FIELD_LENGTH = 1024;
    FILE *fp = fopen(catalog_file, "r");
    if (!fp)
        return NULL;
    Book *catalog = (Book *)malloc(lines * sizeof(Book));
    if (catalog == NULL){
        //needs to send some sort of error
        return NULL;
    }
    char row[MAX_FIELD_LENGTH];
    int current_book = 0;
    while (fgets(row, sizeof(row), fp) && current_book < lines)
    {

        row[strcspn(row, "\r\n")] = 0;

        if (strlen(row) == 0)
            continue;

        Book *b = &catalog[current_book];
        char *cursor = row;
        char yearbuf[32];
        char *name   = next_csv_field(&cursor, b->name,   sizeof(b->name));
        char *author = next_csv_field(&cursor, b->author, sizeof(b->author));
        char *year   = next_csv_field(&cursor, yearbuf,   sizeof(yearbuf));
        if (!name || !author || !year) continue;       // malformed row -> skip

        b->year           = atoi(year);
        b->availability   = AVAILABLE;
        b->lent_to[0]     = '\0';
        b->really_lent    = 0;
        b->verified_until = 0;
        b->lent_to_lib    = -1;
        pthread_mutex_init(&b->lock, NULL);   // initializing per book mutex
        current_book++;
    }
    fclose(fp);
    return catalog;
}

User *find_user(const char *username) {
    // assumes the lib.users lock is already held by caller
    User *user_ptr = NULL;
    for (int i = 0; i < lib.num_users; i++)
    {
        if (strcmp(lib.users[i]->username, username) == 0)
        {
            user_ptr = lib.users[i];
            break;
        }
    }
    return user_ptr;
}

Book* find_book(const char* title) {
    Book *book_ptr = NULL;
    for (int i = 0; i < lib.num_books; i++)
    {
        if (strcmp(lib.catalog[i].name, title) == 0)
        {
            book_ptr = &lib.catalog[i];
            break;
        }
    }
    return book_ptr;
}

int matches(const Book *b, int by_author, int by_title, int by_year, const char *value) {
    if (by_author) return strstr(b->author, value) != NULL;
    if (by_title)  return strstr(b->name,   value) != NULL;
    if (by_year) {
        char ys[16];
        snprintf(ys, sizeof(ys), "%d", b->year);
        return strstr(ys, value) != NULL;
    }
    return 0;
}
