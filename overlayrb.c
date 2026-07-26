#define _GNU_SOURCE
#include <sched.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sendfile.h>
#include <sys/mount.h>
#include <limits.h>
#include <pwd.h>

#define MAX_USTACK 30

typedef enum Action{
    F_CREATE,
    F_DELETE,
    F_WRITE,
    DIR_CREATE,
    DIR_DELETE,
    DIR_MOD,
    ACT_INV
}Action;

typedef struct Memento{
    char *path;
    char *tpath;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    Action action;
}Memento;

typedef struct HollowMemento{
    Memento **mementos;
    int top;
    int size;
}HollowMemento;

char trashdir[PATH_MAX];

char *upperdir_root;
char *workdir_root;

char *upperdir_home;
char *workdir_home;

char *mergeddir;

HollowMemento *ustack[MAX_USTACK];
int utop = -1;

void reap_dir(const char *dir_path, const char *upperdir, const char *base, HollowMemento *holmem);
Action find_action(const char *upper_path, const char *upperdir, const char *base);
Memento *create_mem(Action action, const char *upper_path, const char *upperdir, const char *base);
int commit(const char *upper_path, Action action, const char *upperdir, const char *base);
void undo_mem(Memento *mem);

int push_memstack(HollowMemento *holmem, Memento *mem);
void push_ustack(HollowMemento *holmem);
void pop_ustack();
void free_holmem(HollowMemento *holmem);
void free_mem(Memento *mem);

char *trash(const char *lower_path);
int fcopy(const char *src, const char *dest);
void get_lowpath(const char *up_path, char *low_path, size_t low_sz, const char *base, const char *upperdir);
int drop_root();

int bind_essential(){
    char bdir[PATH_MAX];
    snprintf(bdir, strlen(mergeddir) + 10, "%s/sys", mergeddir);
    mkdir(bdir, 0700);
    if(mount("/sys", bdir, NULL, MS_BIND | MS_REC, NULL) != 0){
        perror("hsh: failed to bind essential");
        return 1;
    }

    snprintf(bdir, strlen(mergeddir) + 10, "%s/dev", mergeddir);
    mkdir(bdir, 0700);
    if(mount("/dev", bdir, NULL, MS_BIND | MS_REC, NULL) != 0){
        perror("hsh: failed to bind essential");
        return 1;
    }

    snprintf(bdir, strlen(mergeddir) + 10, "%s/run", mergeddir);
    mkdir(bdir, 0700);
    if(mount("/run", bdir, NULL, MS_BIND | MS_REC, NULL) != 0){
        perror("hsh: failed to bind essential");
        return 1;
    }

    snprintf(bdir, strlen(mergeddir) + 10, "%s/tmp", mergeddir);
    mkdir(bdir, 0700);
    if(mount("/tmp", bdir, NULL, MS_BIND | MS_REC, NULL) != 0){
        perror("hsh: failed to bind essential");
        return 1;
    }

    snprintf(bdir, strlen(mergeddir) + 10, "%s/proc", mergeddir);
    mkdir(bdir, 0700);
    if(mount("/proc", bdir, NULL, MS_BIND | MS_REC, NULL) != 0){
        perror("hsh: failed to bind essential");
        return 1;
    }

    return 0;
}

int init_child_ovl(){
    char cwd_buf[PATH_MAX];

    if(getcwd(cwd_buf, sizeof(cwd_buf)) == NULL){
        fprintf(stderr, "hsh: failed to get current directory\n");
        return 1;
    }

    if(unshare(CLONE_NEWNS) != 0){
        perror("hsh: unshare failed");
        return 1;
    }

    if(mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) != 0){
        perror("hsh: failed to make mounts private");
        return 1;
    }

    char opts[PATH_MAX * 4];
    snprintf(opts, sizeof(opts), "lowerdir=/,upperdir=%s,workdir=%s", upperdir_root, workdir_root);

    if(mount("overlay", mergeddir, "overlay", 0, opts) != 0){
        perror("hsh: failed to mount overlay");
        return 1;
    }

    char opts_home[PATH_MAX * 4];
    snprintf(opts_home, sizeof(opts_home), "lowerdir=/home,upperdir=%s,workdir=%s", 
             upperdir_home, workdir_home);

    char home_target[PATH_MAX];
    snprintf(home_target, sizeof(home_target), "%s/home", mergeddir);

    mkdir(home_target, 0700);

    if(mount("overlay", home_target, "overlay", 0, opts_home) != 0){
        perror("hsh: failed to mount home overlay");
        return 1;
    }

    if(bind_essential() != 0){
        fprintf(stderr, "hsh: faield to bind essential directories\n");
        return 1;
    }

    if(chdir(mergeddir) != 0){
        fprintf(stderr, "hsh: failed to cd into merged directory\n");
        return 1;
    }

    if(chroot(".") != 0){
        fprintf(stderr, "hsh: chroot failed\n");
        return 1;
    }

    if( chdir(cwd_buf) != 0){
        fprintf(stderr, "failed to chdir into pwd\n");
        return 1;
    }
    setenv("PWD", cwd_buf, 1);

    if(drop_root() != 0){
        fprintf(stderr, "hsh: failed to drop root\n");
        return 1;
    }

    return 0;
}

void init_ovl_dirs(char *shdir){
    size_t sz = strlen(shdir) + 20;

    upperdir_home = malloc(sz);
    upperdir_root = malloc(sz);
    workdir_home = malloc(sz);
    workdir_root = malloc(sz);
    mergeddir = malloc(sz);

    snprintf(upperdir_home, sz, "%s/upper_home", shdir);
    snprintf(upperdir_root, sz, "%s/upper_root", shdir);
    snprintf(workdir_home, sz, "%s/work_home", shdir);
    snprintf(workdir_root, sz, "%s/work_root", shdir);
    snprintf(mergeddir, sz, "%s/merged", shdir);

    mkdir(workdir_root, 0700);
    mkdir(workdir_home, 0700);
    mkdir(upperdir_root, 0700);
    mkdir(upperdir_home, 0700);
    mkdir(mergeddir, 0700);
}

int init_hsh_dirs(){
    srand(time(NULL));

    char *home;
    char *sudo_user = getenv("SUDO_USER");
    struct passwd *pw = getpwnam(sudo_user);
    if(pw && pw->pw_dir){
        home = pw->pw_dir;
    }
    else{
        fprintf(stderr, "hsh: failed to get home directory\n");
        return 1;
    }

    char shell_dir[PATH_MAX];
    snprintf(shell_dir, sizeof(shell_dir), "%s/.local/share/hsh", home);

    struct stat st;
    if(stat(shell_dir, &st) != 0){
        mkdir(shell_dir, 0700);
    }
    else if(S_ISDIR(st.st_mode) == 0){
        fprintf(stderr, "hsh: Failed to create shell trash directory. A file with the same name exists\n");
        return 1;
    }

    init_ovl_dirs(shell_dir);

    snprintf(trashdir, sizeof(trashdir), "%s/.local/share/hsh/trash", home);

    if(stat(trashdir, &st) != 0){
        mkdir(trashdir, 0700);
    }
    else if(S_ISDIR(st.st_mode) == 0){
        fprintf(stderr, "hsh: Failed to create shell trash directory. A file with the same name exists\n");
        return 1;
    }

    return 0;
}

void reap_overlay(){
    HollowMemento *holmem = calloc(1, sizeof(HollowMemento));
    holmem->top = -1;

    reap_dir(upperdir_root, upperdir_root, "/", holmem);
    reap_dir(upperdir_home, upperdir_home, "/home", holmem);

    if(holmem->top >= 0){
        push_ustack(holmem);
    }
    else{
        free_holmem(holmem);
    }
}

void reap_dir(const char *dir_path, const char *upperdir, const char *base, HollowMemento *holmem){
    DIR *dir = opendir(dir_path);

    struct dirent *entry = NULL;
    while((entry = readdir(dir)) != NULL){
        size_t sz = strlen(dir_path) + strlen(entry->d_name) + 5;
        char abs_path[sz];
        snprintf(abs_path, sz, "%s/%s", dir_path, entry->d_name);

        if(entry->d_type == DT_DIR){
            if(strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0){
                Action action = find_action(abs_path, upperdir, base);
                Memento *memento = create_mem(action, abs_path, upperdir, base);
                push_memstack(holmem, memento);
                commit(abs_path, action, upperdir, base);

                reap_dir(abs_path, upperdir, base, holmem);

                rmdir(abs_path);
            }
        }
        else{
            Action action = find_action(abs_path, upperdir, base);
            Memento *memento = create_mem(action, abs_path, upperdir, base);
            push_memstack(holmem, memento);
            commit(abs_path, action, upperdir, base);
            unlink(abs_path);
        }
    }

    closedir(dir);
}

//upper_path is for stat-ing files in upperdir, lower_path is for stat-ing files in lowerdir
Action find_action(const char *upper_path, const char *upperdir, const char *base){
    size_t lower_sz = strlen(upper_path); //this will leave some extra space
    char lower_path[lower_sz];

    get_lowpath(upper_path, lower_path, lower_sz, base, upperdir);

    struct stat st_upper, st_lower;
    if(lstat(upper_path, &st_upper) != 0){
        return ACT_INV;
    }

    if(lstat(lower_path, &st_lower) != 0){
        if(S_ISDIR(st_upper.st_mode)){
            return DIR_CREATE;
        }
        else{
            return F_CREATE;
        }
    }
    else if(S_ISCHR(st_upper.st_mode) && (major(st_upper.st_rdev) == 0 && minor(st_upper.st_rdev) == 0)){
        if(S_ISDIR(st_lower.st_mode)){
            return DIR_DELETE;
        }
        else{
            return F_DELETE;
        }
    }
    else{
        if(S_ISDIR(st_upper.st_mode)){
            return DIR_MOD;
        }
        else{
            return F_WRITE;
        }
    }
}

Memento *create_mem(Action action, const char *upper_path, const char *upperdir, const char *base){
    Memento *mem = calloc(1, sizeof(Memento));

    size_t lower_sz = strlen(upper_path);
    char lower_path[lower_sz];

    get_lowpath(upper_path, lower_path, lower_sz, base, upperdir);

    mem->path = strdup(lower_path);
    mem->action = action;
    switch(action){
        case F_CREATE:
            return mem;

        case DIR_CREATE:
            return mem;;

        case F_WRITE:
        case F_DELETE:{
            mem->tpath = trash(lower_path);

            struct stat st;
            if(lstat(lower_path, &st) != 0){
                return NULL;
            }

            mem->mode = st.st_mode & 07777;
            mem->uid = st.st_uid;
            mem->gid = st.st_gid;

            return mem;
        }

        case DIR_DELETE:
        case DIR_MOD:{
            struct stat st;
            if(lstat(lower_path, &st) != 0){
                free(mem);
                return NULL;
            }

            mem->mode = st.st_mode & 07777;
            mem->uid = st.st_uid;
            mem->gid = st.st_gid;

            return mem;
        }

        case ACT_INV:
            return NULL;
    }

    return NULL;
}

int commit(const char *upper_path, Action action, const char *upperdir, const char *base){
    size_t lower_sz = strlen(upper_path);
    char lower_path[lower_sz];

    get_lowpath(upper_path, lower_path, lower_sz, base, upperdir);

    struct stat st;
    if(lstat(upper_path, &st) != 0){
        return 1;
    }

    switch(action){
        case F_CREATE:{
            if(fcopy(upper_path, lower_path) != 0){
                return 1;
            }

            mode_t mode = st.st_mode & 07777;
            chmod(lower_path, mode);
            chown(lower_path, st.st_uid, st.st_gid);
            break;
        }

        case F_DELETE:
            unlink(lower_path);
            break;

        case DIR_DELETE:
            rmdir(lower_path);
            break;

        case DIR_CREATE:{
            mode_t mode = st.st_mode & 07777;
            mkdir(lower_path, mode);
            chown(lower_path, st.st_uid, st.st_gid);
            break;
        }

        case DIR_MOD:{
            mode_t mode = st.st_mode & 07777;
            chmod(lower_path, mode);
            chown(lower_path, st.st_uid, st.st_gid);
            break;
        }

        case F_WRITE:{
            if(fcopy(upper_path, lower_path) != 0){
                return 1;
            }

            chmod(lower_path, st.st_mode & 07777);
            chown(lower_path, st.st_uid, st.st_gid);
            break;
        }

        case ACT_INV:
            return 1;
    }

    return 0;
}

int undo(char **noop){
    (void)noop;

    if(utop < 0){
        return 1;
    }

    HollowMemento *holmem = ustack[utop];

    while(holmem->top >= 0){
        undo_mem(holmem->mementos[holmem->top]);
        holmem->top--;
    }

    pop_ustack();

    return 0;
}

void undo_mem(Memento *mem){
    switch(mem->action){
        case F_CREATE:
            unlink(mem->path);
            break;

        case DIR_CREATE:
            rmdir(mem->path);
            break;

        case F_DELETE:
            fcopy(mem->tpath, mem->path);
            lchown(mem->path, mem->uid, mem->gid);
            chmod(mem->path, mem->mode);
            break;

        case DIR_DELETE:
            mkdir(mem->path, mem->mode);
            chown(mem->path, mem->uid, mem->gid);
            chmod(mem->path, mem->mode);
            break;

        case DIR_MOD:
            chown(mem->path, mem->uid, mem->gid);
            chmod(mem->path, mem->mode);
            break;

        case F_WRITE:
            fcopy(mem->tpath, mem->path);
            lchown(mem->path, mem->uid, mem->gid);
            chmod(mem->path, mem->mode);
            break;

        case ACT_INV:
            break;
    }
}

int push_memstack(HollowMemento *holmem, Memento *mem){
    if(holmem->top >= holmem->size - 1){
        holmem->size = holmem->size == 0 ? 4 : holmem->size * 2;

        Memento **tmp = realloc(holmem->mementos, sizeof(Memento*) * holmem->size);
        if(tmp == NULL){
            return 1;
        }

        holmem->mementos = tmp;
    }

    holmem->top++;
    holmem->mementos[holmem->top] = mem;

    return 0;
}

void push_ustack(HollowMemento *holmem){
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

void pop_ustack(){
    if(utop < 0) return;

    free_holmem(ustack[utop]);
    utop--;
}

void free_holmem(HollowMemento *holmem){
    while(holmem->top >= 0){
        free_mem(holmem->mementos[holmem->top]);
        holmem->top--;
    }

    free(holmem->mementos);
    free(holmem);
}

void free_mem(Memento *mem){
    if(mem->path != NULL){
        unlink(mem->tpath);
    }

    free(mem->path);
    free(mem->tpath);
    free(mem);
}

char *trash(const char *lower_path){
    char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char rname[9];

    for(int i = 0; i < 8; i++){
        int rindex = rand() % strlen(chars);
        rname[i] = chars[rindex];
    }

    rname[8] = '\0';

    size_t sz = strlen(trashdir) + strlen(rname) + 5;
    char tfile[sz];
    snprintf(tfile, sz, "%s/%s", trashdir, rname);

    if(fcopy(lower_path, tfile) != 0){
        tfile[0] = '\0';
        return NULL;
    }

    return strdup(tfile);
}

int fcopy(const char *src, const char *dest){
    struct stat fstat;
    if(stat(src, &fstat) != 0){
        return 1;
    }

    int src_fd = open(src, O_RDONLY);
    int dest_fd = open(dest, O_TRUNC | O_WRONLY | O_CREAT, 0600);
    off_t copied = 0;
    off_t remaining = fstat.st_size;

    while(remaining > 0){
        ssize_t written = sendfile(dest_fd, src_fd, &copied, remaining);
        remaining -= written;
    }

    close(src_fd);
    close(dest_fd);

    return 0;
}

void get_lowpath(const char *up_path, char *low_path, size_t low_sz, const char *base, const char *upperdir){
    if(strcmp(base, "/") == 0){
        snprintf(low_path, low_sz, "%s", up_path + strlen(upperdir));
    }
    else{
        snprintf(low_path, low_sz, "%s%s", base, up_path + strlen(upperdir));
    }
}

int drop_root(){
    char *uidstr = getenv("SUDO_UID");
    if(uidstr == NULL){
        fprintf(stderr, "hsh:  faield to get sudo uid\n");
        return 1;
    }

    uid_t uid = strtoul(uidstr, NULL, 10);

    char *gidstr = getenv("SUDO_GID");
    if(gidstr == NULL){
        fprintf(stderr, "hsh:  faield to get sudo gid\n");
        return 1;
    }

    gid_t gid = strtoul(gidstr, NULL, 10);


    if(setgid(gid) != 0){
        perror("hsh: setgid failed");
        return 1;
    }

    if(setuid(uid) != 0){
        perror("hsh: setuid failed");
        return 1;
    }

    return 0;
}

void ovl_cleanup(){
    while(utop >= 0){
        pop_ustack();
    }

    free(upperdir_home);
    free(upperdir_root);
    free(workdir_home);
    free(workdir_root);
    free(mergeddir);
}
