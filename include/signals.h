#ifndef SIGNALS_H
#define SIGNALS_H

// 设置信号处理器
void setup_signal_handlers();
void register_locked_file(int fd, const char* path, bool is_main);
void unregister_locked_file(int fd);
int acquire_lock(const Config *cfg, RuntimeState *state);
void release_lock(RuntimeState *state);
#endif // SIGNALS_H