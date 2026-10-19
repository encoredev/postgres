/*
 * The part of e2fsprogs' libuuid API that contrib/uuid-ossp uses, for
 * Windows, which has no UUID library that PostgreSQL builds with.  uuid.c
 * implements it with the RPC runtime's UuidCreate and UuidCreateSequential.
 * encore/deps.sh builds it.
 */
#ifndef ENCORE_UUID_UUID_H
#define ENCORE_UUID_UUID_H

/* rpcdce.h, if included, defines uuid_t as its UUID struct */
#ifdef uuid_t
#undef uuid_t
#endif

typedef unsigned char uuid_t[16];

#ifdef __cplusplus
extern "C"
{
#endif

/* A random (version 4) UUID */
extern void uuid_generate(uuid_t out);
extern void uuid_generate_random(uuid_t out);

/* A time and node based (version 1) UUID */
extern void uuid_generate_time(uuid_t out);

/* The 36 characters of the UUID in lower case, and a terminating NUL */
extern void uuid_unparse(const uuid_t uu, char *out);

#ifdef __cplusplus
}
#endif

#endif							/* ENCORE_UUID_UUID_H */
