extern "C" int psych_add(int a, int b);
extern "C" void psych_print_hello();

// Используем _start вместо main, чтобы избежать зависимости от crt0
extern "C" void _start() {
    psych_print_hello();
    int result = psych_add(5, 7);
    while(1) {} 
}