#ifndef OVERLAYRB_H
#define OVERLAYRB_H

#include <unistd.h>

int init_child_ovl(uid_t uid, gid_t gid);

int init_ovl_dirs();
void reap_overlay();
int undo(char **noop);

void ovl_cleanup();

#endif
