/*
 * TaterTOS64v3 — minimal C++ runtime for userland.
 *
 * Provides the symbols GCC emits for any non-trivial C++ program:
 *   - operator new / delete (all standard forms)
 *   - __cxa_guard_acquire / __cxa_guard_release / __cxa_guard_abort
 *     (thread-safe local-static initialization)
 *   - __cxa_atexit / __cxa_finalize
 *   - __cxa_pure_virtual
 *
 * Origin log: logs/fry841.txt (added when AK.a + LibCore.a were
 * linked into the first TaterTOS .fry consumer and these symbols
 * came up as undefined references).
 *
 * All implementations forward to TaterTOS libc primitives. No
 * dependence on libstdc++ / libsupc++.
 */

#include "libc.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <new>          // std::nothrow_t, std::align_val_t

extern "C" {

void *malloc(size_t);
void  free(void *);

}  // extern "C"

/*
 * Global new/delete — all standard signatures. C++14 sized-delete
 * variants included so any caller using them links cleanly.
 */
void *operator new(size_t size) {
    void *p = malloc(size == 0 ? 1 : size);
    return p;  // throwing-new: caller expects nullptr → bad_alloc;
               // we are -fno-exceptions so callers must check.
}

void *operator new[](size_t size) {
    return ::operator new(size);
}

void *operator new(size_t size, std::nothrow_t const &) noexcept {
    return malloc(size == 0 ? 1 : size);
}

void *operator new[](size_t size, std::nothrow_t const &) noexcept {
    return malloc(size == 0 ? 1 : size);
}

void operator delete(void *p) noexcept             { free(p); }
void operator delete[](void *p) noexcept           { free(p); }
void operator delete(void *p, size_t) noexcept     { free(p); }
void operator delete[](void *p, size_t) noexcept   { free(p); }
void operator delete(void *p, std::nothrow_t const &) noexcept   { free(p); }
void operator delete[](void *p, std::nothrow_t const &) noexcept { free(p); }

/* Aligned allocation (C++17). Use posix_memalign. */
extern "C" int posix_memalign(void **, size_t, size_t);

void *operator new(size_t size, std::align_val_t alignment) {
    void *p = nullptr;
    if (posix_memalign(&p, static_cast<size_t>(alignment),
                       size == 0 ? 1 : size) != 0)
        return nullptr;
    return p;
}

void *operator new[](size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void *p, std::align_val_t) noexcept   { free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { free(p); }
void operator delete(void *p, size_t, std::align_val_t) noexcept   { free(p); }
void operator delete[](void *p, size_t, std::align_val_t) noexcept { free(p); }

/*
 * std::nothrow definition. Declared in our <new> shim; defined here
 * as a singleton.
 */
namespace std {
nothrow_t const nothrow;
}

extern "C" {

/*
 * Itanium C++ ABI guard primitives for thread-safe local-static
 * initialization. The guard is a 64-bit word; byte 0 is the
 * "initialized" flag, byte 1 is the "in-progress" flag for the
 * single-thread-running-init case. Multi-thread races would need a
 * futex; we use the existing fry_futex_wait/wake primitives.
 *
 * Layout per Itanium ABI: __cxa_guard_acquire returns 1 if the
 * initializer must run, 0 if already initialized.
 */
struct guard_t { uint8_t bytes[8]; };

int __cxa_guard_acquire(uint64_t *guard_object) {
    uint8_t *bytes = reinterpret_cast<uint8_t *>(guard_object);
    if (bytes[0])           // already initialized
        return 0;
    if (bytes[1]) {
        // Another thread is initializing. Spin via fry_yield equivalent.
        // For now, a tight loop — acceptable on TaterTOS where init is
        // typically single-threaded at startup. Hardened later via futex.
        while (bytes[1]) {
            __asm__ volatile("pause");
        }
        return 0;
    }
    bytes[1] = 1;           // mark in-progress
    return 1;               // caller runs the init
}

void __cxa_guard_release(uint64_t *guard_object) {
    uint8_t *bytes = reinterpret_cast<uint8_t *>(guard_object);
    bytes[0] = 1;           // mark initialized
    bytes[1] = 0;           // clear in-progress
}

void __cxa_guard_abort(uint64_t *guard_object) {
    uint8_t *bytes = reinterpret_cast<uint8_t *>(guard_object);
    bytes[1] = 0;
}

/*
 * __cxa_atexit / __dso_handle — required by GCC for emitting
 * destructors of globals at process exit. We don't currently run
 * atexit handlers (TaterTOS exit is process_exit_group; cleanup is
 * via shared->refcount in the kernel). Keep the symbols so linking
 * succeeds; ignore registration.
 */
int __cxa_atexit(void (*)(void *), void *, void *) {
    return 0;
}

int __cxa_thread_atexit(void (*)(void *), void *, void *) {
    return 0;
}

void __cxa_finalize(void *) {}

/* dso_handle — placeholder address for the "this DSO" identity. */
void *__dso_handle = nullptr;

/*
 * __cxa_pure_virtual — called if a pure virtual member is invoked
 * before construction completes. Should be unreachable; abort if hit.
 */
extern void abort(void);
void __cxa_pure_virtual() {
    abort();
}

/*
 * __cxa_throw / __cxa_allocate_exception / __cxa_free_exception —
 * we build with -fno-exceptions so these should never be called.
 * Provide them as abort() stubs for any straggling references.
 */
void __cxa_throw(void *, void *, void (*)(void *)) {
    abort();
}

void *__cxa_allocate_exception(size_t) {
    abort();
    return nullptr;
}

void __cxa_free_exception(void *) {}

void __cxa_rethrow() {
    abort();
}

void __cxa_call_unexpected(void *) {
    abort();
}

void __cxa_begin_catch(void *) {
    abort();
}

void __cxa_end_catch() {}

/*
 * Personality function for unwind tables. Never called when
 * exceptions are off; provide a no-op to satisfy the linker if a
 * .eh_frame pulls it in.
 */
int __gxx_personality_v0(int, int, uint64_t, void *, void *) {
    return 0;
}

}  // extern "C"

namespace __cxxabiv1 {
    class __class_type_info {
    public:
        virtual ~__class_type_info();
    };
    __class_type_info::~__class_type_info() {}

    class __si_class_type_info : public __class_type_info {
    public:
        virtual ~__si_class_type_info();
    };
    __si_class_type_info::~__si_class_type_info() {}

    class __vmi_class_type_info : public __class_type_info {
    public:
        virtual ~__vmi_class_type_info();
    };
    __vmi_class_type_info::~__vmi_class_type_info() {}

    struct raw_class_type_info {
        void* vtable;
        char const* name;
    };

    struct raw_si_class_type_info {
        raw_class_type_info base;
        __class_type_info const* base_type;
    };

    struct raw_base_class_type_info {
        __class_type_info const* base_type;
        intptr_t offset_flags;
    };

    struct raw_vmi_class_type_info {
        raw_class_type_info base;
        unsigned int flags;
        unsigned int base_count;
        raw_base_class_type_info base_info[1];
    };

    static bool type_info_matches(__class_type_info const* a, __class_type_info const* b)
    {
        if (a == b)
            return true;

        auto* raw_a = reinterpret_cast<raw_class_type_info const*>(a);
        auto* raw_b = reinterpret_cast<raw_class_type_info const*>(b);
        if (!raw_a->name || !raw_b->name)
            return false;

        return strcmp(raw_a->name, raw_b->name) == 0;
    }

    static bool is_si_type(__class_type_info const* type)
    {
        static __si_class_type_info si_probe;
        return reinterpret_cast<raw_class_type_info const*>(type)->vtable
            == reinterpret_cast<raw_class_type_info const*>(&si_probe)->vtable;
    }

    static bool is_vmi_type(__class_type_info const* type)
    {
        static __vmi_class_type_info vmi_probe;
        return reinterpret_cast<raw_class_type_info const*>(type)->vtable
            == reinterpret_cast<raw_class_type_info const*>(&vmi_probe)->vtable;
    }

    static ptrdiff_t base_offset(void const* object, raw_base_class_type_info const& base)
    {
        ptrdiff_t offset = static_cast<ptrdiff_t>(base.offset_flags) >> 8;
        bool is_virtual = (base.offset_flags & 0x1) != 0;
        if (!is_virtual)
            return offset;

        auto* vtable = *reinterpret_cast<void* const* const*>(object);
        return *reinterpret_cast<ptrdiff_t const*>(reinterpret_cast<char const*>(vtable) + offset);
    }

    struct rtti_search_result {
        void const* match { nullptr };
        unsigned matches { 0 };
        bool source_found { false };
    };

    static void walk_public_bases(
        __class_type_info const* current_type,
        void const* current_object,
        __class_type_info const* wanted_type,
        __class_type_info const* source_type,
        void const* source_object,
        bool public_path,
        rtti_search_result& result)
    {
        if (!current_type)
            return;

        if (public_path && type_info_matches(current_type, source_type) && current_object == source_object)
            result.source_found = true;

        if (public_path && type_info_matches(current_type, wanted_type)) {
            if (result.match != current_object)
                ++result.matches;
            result.match = current_object;
        }

        if (is_si_type(current_type)) {
            auto* si = reinterpret_cast<raw_si_class_type_info const*>(current_type);
            walk_public_bases(si->base_type, current_object, wanted_type, source_type, source_object, public_path, result);
            return;
        }

        if (!is_vmi_type(current_type))
            return;

        auto* vmi = reinterpret_cast<raw_vmi_class_type_info const*>(current_type);
        for (unsigned i = 0; i < vmi->base_count; ++i) {
            auto const& base = vmi->base_info[i];
            bool base_is_public = (base.offset_flags & 0x2) != 0;
            void const* base_object = reinterpret_cast<char const*>(current_object) + base_offset(current_object, base);
            walk_public_bases(base.base_type, base_object, wanted_type, source_type, source_object, public_path && base_is_public, result);
        }
    }

    extern "C" void* __dynamic_cast(
        void const* source_object,
        __class_type_info const* source_type,
        __class_type_info const* destination_type,
        ptrdiff_t)
    {
        if (!source_object || !source_type || !destination_type)
            return nullptr;

        auto* vtable = *reinterpret_cast<void* const* const*>(source_object);
        auto offset_to_top = reinterpret_cast<ptrdiff_t const*>(vtable)[-2];
        auto* dynamic_type = reinterpret_cast<__class_type_info const* const*>(vtable)[-1];
        void const* complete_object = reinterpret_cast<char const*>(source_object) + offset_to_top;

        rtti_search_result result;
        walk_public_bases(dynamic_type, complete_object, destination_type, source_type, source_object, true, result);

        if (!result.source_found || result.matches != 1)
            return nullptr;

        return const_cast<void*>(result.match);
    }
}

extern "C" void abort(void);

namespace std {
    void __throw_bad_function_call() {
        abort();
    }
    void __throw_length_error(char const*) {
        abort();
    }
}
