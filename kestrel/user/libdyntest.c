/* user/libdyntest.c -- the shared library of user/dyntest.c */
int dyn_counter = 100;                  /* data, relocated by the loader */
__thread int dyn_tls = 7;               /* thread-local storage in a shared object */
static const char *names[] = { "libdyntest" };   /* a pointer that needs a RELATIVE relocation */

int dyn_add(int a, int b) { dyn_counter++; return a + b; }
const char *dyn_name(void) { return names[0]; }
int *dyn_tls_addr(void) { return &dyn_tls; }
