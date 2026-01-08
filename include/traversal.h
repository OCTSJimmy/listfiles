#ifndef TRAVERSAL_H
#define TRAVERSAL_H

#include "config.h"

// 执行主遍历循环
void traverse_files(const Config *cfg, RuntimeState *state);
void traversal_add_pending_tasks(int count);

#endif // TRAVERSAL_H