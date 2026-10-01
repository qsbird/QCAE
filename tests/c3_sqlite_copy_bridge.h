#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* qcae_c3_sqlite_memcpy(void* destination, const void* source, size_t bytes);
void* qcae_c3_sqlite_memmove(void* destination, const void* source, size_t bytes);
void* qcae_c3_sqlite_memcpy_tagged(void* destination,
                                   const void* source,
                                   size_t bytes,
                                   const char* function);
void* qcae_c3_sqlite_memmove_tagged(void* destination,
                                    const void* source,
                                    size_t bytes,
                                    const char* function);
void* qcae_c3_sqlite_memcpy_aggregate(void* destination,
                                      const void* source,
                                      size_t bytes,
                                      const char* function);
void* qcae_c3_sqlite_realloc(void* allocation, size_t bytes, const char* function);
typedef void (*qcae_c3_sqlite_profile_visitor)(
    void* context, const char* function, size_t bytes, size_t calls, int aggregate);
void qcae_c3_sqlite_visit_copy_profile(void* context, qcae_c3_sqlite_profile_visitor visitor);
void qcae_c3_sqlite_encoded(size_t bytes);
/* Returns zero unless the linked SQLite is the audited 3.51.0 build. */
int qcae_c3_sqlite_observer_begin(void);

#ifdef __cplusplus
}
#endif
