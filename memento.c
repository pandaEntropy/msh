#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <libgen.h>
#include <time.h>
#include <sys/sendfile.h>
#include <fcntl.h>

#define MAX_USTACK 40

typedef enum Cmdtype{
    CMD_MOVE,
    CMD_DELETE_FILE,
    CMD_DELETE_DIR,
    CMD_INTERNAL,
    CMD_CREATE_FILE,
    CMD_CREATE_DIR,
    CMD_NOUNDO
}Cmdtype;

typedef struct CmdRule{
    char *name;
    Cmdtype type;
}CmdRule;

typedef struct TrashFile{
    struct timeval times[2];
    char *path;
    char *trash_path;
    unsigned int permissions;
}TrashFile;

typedef struct CreateMemento{
    struct timeval (*dir_times)[2];
    char **parent_dirs;

    char **paths;
    int num_paths;

    bool is_dir;
    void *root;
}CreateMemento;

typedef struct MoveMemento{
    char *source;
    char *dest;

    char *source_pdir;
    char *dest_pdir;

    struct timeval spdir_time[2];
    struct timeval dpdir_time[2];

    void *root;
}MoveMemento;

typedef struct DeleteMemento{
    TrashFile *tfiles;

    char **parent_dirs;
    int num_paths;

    struct timeval (*parent_times)[2];
    void *root;
}DeleteMemento;

typedef struct InternalMemento{
    char *cwd;
    void *root;
}InternalMemento;

typedef struct Arena{
    size_t offset;
    size_t size;
    char *root;
}Arena;

typedef enum MemType{
    MEM_CREATE,
    MEM_MOVE,
    MEM_DELETE,
    MEM_INTERNAL,
    MEM_INV
}MemType;

typedef struct HollowMemento{
    union{
        CreateMemento *creatmem;
        MoveMemento *movmem;
        DeleteMemento *delmem;
        InternalMemento *intmem;
    }mem;
    MemType memtype;
}HollowMemento;

int init_memento();
void create_holmem(char **args, HollowMemento *holmem);
void push(HollowMemento holmem);
int undo(char **args);
void free_holmem(HollowMemento holmem);
void mem_cleanup();

void push(HollowMemento holmem);
void get_timestamps(char *name, struct timeval *time);
void trash(char *file, char *tfile);

static HollowMemento ustack[MAX_USTACK];
static int utop = -1;

static char trash_path[PATH_MAX];

CmdRule rule_table[] = {
    {"mv", CMD_MOVE}, {"rename", CMD_MOVE}, {"cd", CMD_INTERNAL},
    {"rm", CMD_DELETE_FILE}, {"rmdir", CMD_DELETE_DIR}, {"unlink", CMD_DELETE_FILE},
    {"mkdir", CMD_CREATE_DIR}, {"touch", CMD_CREATE_FILE}, {"ln", CMD_CREATE_FILE}
};

void *ar_alloc(Arena *arena, size_t size, size_t alignment){
    unsigned long cur_ptr = (unsigned long)arena->root + arena->offset;
    unsigned long aligned_ptr = (cur_ptr + (alignment - 1)) & ~(alignment - 1);

    size_t new_offset = aligned_ptr - (unsigned long)arena->root + size;

    if(new_offset > arena->size){
        return NULL;
    }

    arena->offset = new_offset;
    return (void*)aligned_ptr;
}

Cmdtype get_cmdtype(char *name){
    int length = sizeof(rule_table) / sizeof(CmdRule); 
    for(int i = 0; i < length; i++){
        if(strcmp(name, rule_table[i].name) == 0) 
            return rule_table[i].type;
    }

    return CMD_NOUNDO;
}

void create_creatmem(char **args, size_t char_count, int argc, HollowMemento *holmem, Cmdtype type){
    int num_paths = argc - 1;

    size_t creatmem_sz = sizeof(CreateMemento);
    size_t path_sz = num_paths * sizeof(char *);
    size_t parent_dirs_sz = num_paths * sizeof(char *);
    size_t dir_times_sz = sizeof(struct timeval) * num_paths * 2;

    size_t args_sz = char_count;
    size_t dir_sz = char_count;

    size_t total_sz = creatmem_sz + path_sz + parent_dirs_sz + dir_times_sz + args_sz + dir_sz + 64;
    Arena arena = {0};
    arena.root = malloc(total_sz);
    arena.size = total_sz;

    CreateMemento *creatmem = ar_alloc(&arena, creatmem_sz, 8);
    creatmem->root = arena.root;
    creatmem->paths = ar_alloc(&arena, path_sz, 8);
    creatmem->parent_dirs = ar_alloc(&arena, parent_dirs_sz, 8);
    creatmem->dir_times = ar_alloc(&arena, dir_times_sz, 8);
    creatmem->is_dir = (type == CMD_CREATE_DIR);
    creatmem->num_paths = num_paths;

    for(int i = 0; args[i + 1] != NULL; i++){
        char *arg = args[i + 1];

        char *arena_str = ar_alloc(&arena, strlen(arg) + 1, 1);
        strcpy(arena_str, arg);
        creatmem->paths[i] = arena_str;

        char tmp_path[PATH_MAX];
        snprintf(tmp_path, sizeof(tmp_path), "%s", arg);

        char *dir = dirname(tmp_path);
        char *dir_str = ar_alloc(&arena, strlen(dir) + 1, 1);
        strcpy(dir_str, dir);
        creatmem->parent_dirs[i] = dir_str;

        get_timestamps(dir, creatmem->dir_times[i]);
    }

    holmem->memtype = MEM_CREATE;
    holmem->mem.creatmem = creatmem;
}

void create_movmem(char **args, size_t char_count, int argc, HollowMemento *holmem){
    (void)argc;
    (void)char_count;

    size_t movmem_sz = sizeof(MoveMemento);
    size_t str_sz = (strlen(args[1]) + strlen(args[2]) + 2) * 2;

    size_t total_sz = movmem_sz + str_sz + 64;

    Arena arena = {0};
    arena.root = malloc(total_sz);
    arena.size = total_sz;

    MoveMemento *movmem = ar_alloc(&arena, movmem_sz, 8);
    movmem->root = arena.root;

    char *src_str = ar_alloc(&arena, strlen(args[1]) + 1, 1);
    strcpy(src_str, args[1]);
    movmem->source = src_str;

    char *dest_str = ar_alloc(&arena, strlen(args[2]) + 1, 1);
    strcpy(dest_str, args[2]);
    movmem->dest = dest_str;

    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s", args[1]);
    char *dir = dirname(tmp_path);
    char *arena_dir = ar_alloc(&arena, strlen(dir) + 1, 1);
    strcpy(arena_dir, dir);
    movmem->source_pdir = arena_dir;

    get_timestamps(dir, movmem->spdir_time);

    snprintf(tmp_path, sizeof(tmp_path), "%s", args[2]);
    dir = dirname(tmp_path);
    arena_dir = ar_alloc(&arena, strlen(dir) + 1, 1);
    strcpy(arena_dir, dir);
    movmem->dest_pdir = arena_dir;

    get_timestamps(dir, movmem->dpdir_time);

    holmem->memtype = MEM_MOVE;
    holmem->mem.movmem = movmem;
}

void create_delmem(char **args, size_t char_count, int argc, HollowMemento *holmem, Cmdtype type){
    int num_paths = argc - 1;

    size_t delmem_sz = sizeof(DeleteMemento);
    size_t tfiles_sz = num_paths * sizeof(TrashFile);
    size_t parent_dirs_sz = num_paths * sizeof(char *);
    size_t parent_times_sz = sizeof(struct timeval) * num_paths * 2;

    size_t arg_sz = char_count;
    size_t dir_sz = char_count;
    size_t trash_sz = num_paths * (strlen(trash_path) + 10);

    size_t total_sz = delmem_sz + tfiles_sz + parent_dirs_sz + parent_times_sz + arg_sz + dir_sz + trash_sz + 64;

    Arena arena = {0};
    arena.root = malloc(total_sz);
    arena.size = total_sz;

    DeleteMemento *delmem = ar_alloc(&arena, delmem_sz, 8);
    delmem->root = arena.root;
    delmem->tfiles = ar_alloc(&arena, tfiles_sz, 8);
    delmem->parent_dirs = ar_alloc(&arena, parent_dirs_sz, 8);
    delmem->parent_times = ar_alloc(&arena, parent_times_sz, 8);
    delmem->num_paths = num_paths;

    for(int i = 0; args[i + 1] != NULL; i++){
        char *arg = args[i + 1];

        char *arena_str = ar_alloc(&arena, strlen(arg) + 1, 1);
        strcpy(arena_str, arg);
        delmem->tfiles[i].path = arena_str;

        if(type == CMD_DELETE_FILE){
            char tmp_trash_path[PATH_MAX];
            trash(arg, tmp_trash_path);

            if(tmp_trash_path[0] == '\0'){
                free(arena.root);
                return;
            }

            char *t_str = ar_alloc(&arena, strlen(tmp_trash_path) + 1, 1);
            strcpy(t_str, tmp_trash_path);
            delmem->tfiles[i].trash_path = t_str;
        }
        else{
            delmem->tfiles[i].trash_path = NULL;
        }

        char tmp_dir_path[PATH_MAX];
        snprintf(tmp_dir_path, sizeof(tmp_dir_path), "%s", arg);

        char *dir = dirname(tmp_dir_path);
        char *arena_dir = ar_alloc(&arena, strlen(dir) + 1, 1);
        strcpy(arena_dir, dir);
        delmem->parent_dirs[i] = arena_dir;

        struct stat st;
        if(stat(arg, &st) != 0){
            free(arena.root);
            return;
        }

        delmem->tfiles[i].permissions = st.st_mode & 07777;

        get_timestamps(dir, delmem->parent_times[i]);

        get_timestamps(arg, delmem->tfiles[i].times);
    }

    holmem->memtype = MEM_DELETE;
    holmem->mem.delmem = delmem;
}

void create_intmem(char **args, int arg_idx, int argc, HollowMemento *holmem){
    (void)args;
    (void)arg_idx;
    (void)argc;

    InternalMemento *intmem = malloc(sizeof(InternalMemento));

    char buf[PATH_MAX];
    if(getcwd(buf, sizeof(buf)) != NULL){
        intmem->cwd = strdup(buf);
    }

    holmem->memtype = MEM_INTERNAL;
    holmem->mem.intmem = intmem;
}

void create_holmem(char **args, HollowMemento *holmem){
    if(args == NULL || args[0] == NULL){
        return;
    }

    holmem->memtype = MEM_INV;

    Cmdtype type = get_cmdtype(args[0]);

    int argc = 1;
    size_t char_count = 0;
    for(int i = 1; args[i] != NULL; i++){
        if(args[i][0] == '-'){
            type = CMD_NOUNDO;
            break;
        }
        char_count += strlen(args[i]) + 1;
        argc++;
    }

    switch(type){
        case CMD_CREATE_FILE:
            create_creatmem(args, char_count, argc, holmem, CMD_CREATE_FILE);
            break;

        case CMD_CREATE_DIR:
            create_creatmem(args, char_count, argc, holmem, CMD_CREATE_DIR);
            break;

        case CMD_MOVE:
            create_movmem(args, char_count, argc, holmem);
            break;

        case CMD_DELETE_FILE:
            create_delmem(args, char_count, argc, holmem, CMD_DELETE_FILE);
            break;

        case CMD_DELETE_DIR:
            create_delmem(args, char_count, argc, holmem, CMD_DELETE_DIR);
            break;

        case CMD_INTERNAL:
            create_intmem(args, char_count, argc, holmem);
            break;

        case CMD_NOUNDO:
            holmem->memtype = MEM_INV;
            return;
    }
}

void free_creatmem(CreateMemento *creatmem){
    free(creatmem->root);
}

void free_movmem(MoveMemento *movmem){
    free(movmem->root);
}

void free_delmem(DeleteMemento *delmem){
    for(int i = 0; i < delmem->num_paths; i++){
        if(delmem->tfiles[i].trash_path !=  NULL){
            unlink(delmem->tfiles[i].trash_path);
        }
    }
    free(delmem->root);
}

void free_intmem(InternalMemento *intmem){
    free(intmem->cwd);

    free(intmem);
}

void free_holmem(HollowMemento holmem){
    switch(holmem.memtype){
        case MEM_CREATE:
            free_creatmem(holmem.mem.creatmem);
            break;

        case MEM_MOVE:
            free_movmem(holmem.mem.movmem);
            break;

        case MEM_DELETE:
            free_delmem(holmem.mem.delmem);
            break;

        case MEM_INTERNAL:
            free_intmem(holmem.mem.intmem);
            break;

        case MEM_INV:
            return;
    }
}

void push(HollowMemento holmem){
    if(holmem.memtype == MEM_INV) return;

    if(utop >= MAX_USTACK - 1){

        free_holmem(ustack[0]);

        for(int i = 0; i < MAX_USTACK - 1; i++){
            ustack[i] = ustack[i + 1];
        }

        utop = MAX_USTACK - 1;

        ustack[utop] = holmem;
    }
    else{
        utop++;
        ustack[utop] = holmem;
    }
}

void pop(){
    if(utop < 0) return;

    free_holmem(ustack[utop]);
    utop--;
}

void get_timestamps(char *name, struct timeval *time){
    struct stat st;
    if(stat(name, &st) != 0){
        return;
    }

    time[0].tv_sec = st.st_atime;
    time[0].tv_usec = 0;

    time[1].tv_sec = st.st_mtime;
    time[1].tv_usec = 0;
}

void undo_creatmem(CreateMemento *creatmem, bool is_dir){
    if(is_dir){
        for(int i = 0; i < creatmem->num_paths; i++){
            rmdir(creatmem->paths[i]);

            utimes(creatmem->parent_dirs[i], creatmem->dir_times[i]);
        }
    }
    else{
        for(int i = 0; i < creatmem->num_paths; i++){
            unlink(creatmem->paths[i]);

            utimes(creatmem->parent_dirs[i], creatmem->dir_times[i]);
        }
    }
}

void undo_movmem(MoveMemento *movmem){
    rename(movmem->dest, movmem->source);

    utimes(movmem->source_pdir, movmem->spdir_time);
    utimes(movmem->dest_pdir, movmem->dpdir_time);
}

void undo_delmem(DeleteMemento *delmem){
    for(int i = 0; i < delmem->num_paths; i++){
        if(delmem->tfiles[i].trash_path != NULL){
            rename(delmem->tfiles[i].trash_path, delmem->tfiles[i].path);
        }
        else{
            mkdir(delmem->tfiles[i].path, 0700);
        }

        chmod(delmem->tfiles[i].path, delmem->tfiles[i].permissions);

        utimes(delmem->tfiles[i].path, delmem->tfiles[i].times);
        utimes(delmem->parent_dirs[i], delmem->parent_times[i]);
    }
}

void undo_intmem(InternalMemento *intmem){
    chdir(intmem->cwd);
}

int undo(char **args){
    (void)args;

    if(utop < 0) return 1;
    HollowMemento holmem = ustack[utop];

    switch(holmem.memtype){
        case MEM_CREATE:
            undo_creatmem(holmem.mem.creatmem, holmem.mem.creatmem->is_dir);
            break;

        case MEM_MOVE:
            undo_movmem(holmem.mem.movmem);
            break;

        case MEM_DELETE:
            undo_delmem(holmem.mem.delmem);
            break;

        case MEM_INTERNAL:
            undo_intmem(holmem.mem.intmem);
            break;

        case MEM_INV:
            break;
    }

    pop();

    return 0;
}

int init_memento(){
    srand(time(NULL));

    char *home = getenv("HOME");
    if(home != NULL){
        char shell_dir[PATH_MAX];
        snprintf(shell_dir, sizeof(shell_dir), "%s/.local/share/hsh", home);

        struct stat st;
        if(stat(shell_dir, &st) != 0){
            mkdir(shell_dir, 0700);
        }
        else if(S_ISDIR(st.st_mode) == 0){
            fprintf(stderr, "hsh: Failed to create shell directory. A file with the same name exists\n");
            return 1;
        }

        snprintf(trash_path, sizeof(trash_path), "%s/.local/share/hsh/trash", home);

        if(stat(trash_path, &st) != 0){
            mkdir(trash_path, 0700);
        }
        else if(S_ISDIR(st.st_mode) == 0){
            fprintf(stderr, "hsh: Failed to create shell trash directory. A file with the same name exists\n");
            return 1;
        }
    }

    return 0;
}

void trash(char *file, char *tfile){
    char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char rname[9];

    for(int i = 0; i < 8; i++){
        int rindex = rand() % strlen(chars);
        rname[i] = chars[rindex];
    }

    rname[8] = '\0';

    snprintf(tfile, PATH_MAX, "%s/%s", trash_path, rname);

    struct stat fstat;
    if(stat(file, &fstat) != 0){
        tfile[0] = '\0';
        return;
    }

    int src_fd = open(file, O_RDONLY);
    int dest_fd = open(tfile, O_TRUNC | O_WRONLY | O_CREAT, 0600);
    off_t copied = 0;
    off_t remaining = fstat.st_size;

    while(remaining > 0){
        ssize_t written = sendfile(dest_fd, src_fd, &copied, remaining);
        remaining -= written;
    }

    close(src_fd);
    close(dest_fd);
}

void mem_cleanup(){
    while(utop >= 0){
        pop();
    }
}
