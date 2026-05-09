// psych_os.h
#pragma once

// Здесь мы объявляем функции, которые программа может вызывать.
// Реализация этих функций (ассемблерные вставки для вызова seL4 IPC или регистров)
// должна быть слинкована с программой, но пока сделаем заглушки.

extern void sys_print(const char* text);
extern void sys_exit(int code);

// В будущем добавишь сюда:
// extern int sys_open(const char* path);
// extern int sys_read(int fd, char* buf, int size);