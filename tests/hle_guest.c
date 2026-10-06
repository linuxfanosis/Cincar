long cincar_puts(const char* s);
long cincar_add(long a, long b);
long cincar_missing(void);

int main(void) {
    cincar_puts("hello from an HLE import");
    if (cincar_add(2, 3) != 5) return 1;
    if (cincar_missing() != -38) return 2;   // -ENOSYS
    return 0;
}
