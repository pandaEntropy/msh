#ifndef OVERLAYRB_H
#define OVERLAYRB_H

int init_child_ovl();

int init_hsh_dirs();
void reap_overlay();
int undo(char **noop);

void ovl_cleanup();

#endif
