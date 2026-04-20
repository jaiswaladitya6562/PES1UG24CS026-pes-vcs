#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }

            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Load index
int index_load(Index *index) {
    memset(index, 0, sizeof(Index));

    FILE *f = fopen(".pes/index", "r");
    if (!f) return 0;

    while (1) {
        IndexEntry e;
        char hash_hex[HASH_HEX_SIZE + 1];

        int ret = fscanf(f, "%o %64s %lu %u %511s",
                         &e.mode, hash_hex,
                         &e.mtime_sec, &e.size, e.path);

        if (ret == EOF) break;

        if (ret == 5) {
            hex_to_hash(hash_hex, &e.hash);
            index->entries[index->count++] = e;
        }
    }

    fclose(f);
    return 0;
}

// Sorting helper
int cmp_entries(const void *a, const void *b) {
    return strcmp(((IndexEntry*)a)->path, ((IndexEntry*)b)->path);
}

// Save index
int index_save(const Index *index) {
    if (index->count < 0 || index->count > MAX_INDEX_ENTRIES)
        return -1;

    mkdir(".pes", 0755);

    IndexEntry temp[MAX_INDEX_ENTRIES];
    memcpy(temp, index->entries, index->count * sizeof(IndexEntry));

    qsort(temp, index->count, sizeof(IndexEntry), cmp_entries);

    FILE *f = fopen(".pes/index.tmp", "w");
    if (!f) return -1;

    for (int i = 0; i < index->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&temp[i].hash, hex);

        fprintf(f, "%o %s %lu %u %s\n",
                temp[i].mode, hex,
                (unsigned long)temp[i].mtime_sec,
                temp[i].size,
                temp[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    rename(".pes/index.tmp", ".pes/index");
    return 0;
}

// Add file
int index_add(Index *index, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    if (size <= 0) {
        fclose(f);
        return -1;
    }

    void *data = malloc(size);
    if (!data) {
        fclose(f);
        return -1;
    }

    if (fread(data, 1, size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);

    ObjectID id;
    if (object_write(OBJ_BLOB, data, size, &id) != 0) {
        free(data);
        return -1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        free(data);
        return -1;
    }

    IndexEntry *existing = index_find(index, path);

    if (existing) {
        existing->hash = id;
        existing->mtime_sec = st.st_mtime;
        existing->size = st.st_size;
        existing->mode = st.st_mode;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) {
            free(data);
            return -1;
        }

        IndexEntry *e = &index->entries[index->count++];
        strcpy(e->path, path);
        e->hash = id;
        e->mtime_sec = st.st_mtime;
        e->size = st.st_size;
        e->mode = st.st_mode;
    }

    free(data);
    return 0;
}