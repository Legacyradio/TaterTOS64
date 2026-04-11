/*
 * cxxrt.cpp -- Minimal C++ runtime for OpenH264 on TaterTOS64v3
 *
 * Provides operator new/delete (forwarded to malloc/free) and the
 * handful of __cxa_* symbols the compiler emits for static init,
 * pure-virtual guards, and atexit.
 *
 * Compiled with -fno-exceptions -fno-rtti so nothing else is needed.
 */

extern "C" {
    void *malloc(unsigned long size);
    void free(void *ptr);
    void fry_exit(int code);
}

/* ---- operator new / delete ---- */

void *operator new(unsigned long size)                  { return malloc(size); }
void *operator new[](unsigned long size)                { return malloc(size); }
void  operator delete(void *p) noexcept                 { free(p); }
void  operator delete[](void *p) noexcept               { free(p); }
void  operator delete(void *p, unsigned long) noexcept   { free(p); }
void  operator delete[](void *p, unsigned long) noexcept { free(p); }

/* ---- C++ ABI stubs ---- */

extern "C" {

void __cxa_pure_virtual(void) {
    /* Pure virtual call = bug.  Halt. */
    fry_exit(99);
}

int __cxa_atexit(void (*)(void *), void *, void *) {
    /* No destructors at exit on a bare-metal OS. */
    return 0;
}

void *__dso_handle = 0;

}
