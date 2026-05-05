#pragma once
#include "common.h"

extern uint8_t patient_stack[4096];
extern uint8_t patient_tls[1024]; // Экспортируем TLS регион для использования в main.cpp

extern uint8_t doctor_stack[4096];
extern uint8_t doctor_tls[1024];

void patient_thread(seL4_CPtr ep, seL4_Word ipc_buf, seL4_CPtr med_ep);
void doctor_thread(seL4_CPtr ep, seL4_Word ipc_buf, seL4_CPtr med_ep);