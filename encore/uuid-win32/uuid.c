/*
 * uuid/uuid.h for Windows; see there.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <rpc.h>

#include "uuid/uuid.h"

/*
 * A UUID's bytes in the order RFC 9562 gives them: a GUID holds its first
 * three fields in the machine's (little-endian) byte order.
 */
static void
from_guid(const UUID *g, unsigned char *out)
{
	out[0] = (unsigned char) (g->Data1 >> 24);
	out[1] = (unsigned char) (g->Data1 >> 16);
	out[2] = (unsigned char) (g->Data1 >> 8);
	out[3] = (unsigned char) g->Data1;
	out[4] = (unsigned char) (g->Data2 >> 8);
	out[5] = (unsigned char) g->Data2;
	out[6] = (unsigned char) (g->Data3 >> 8);
	out[7] = (unsigned char) g->Data3;
	memcpy(out + 8, g->Data4, 8);
}

void
uuid_generate(uuid_t out)
{
	uuid_generate_random(out);
}

void
uuid_generate_random(uuid_t out)
{
	UUID		g;

	/* Fails only if the system can't produce random numbers */
	if (UuidCreate(&g) != RPC_S_OK)
		abort();
	from_guid(&g, out);
}

void
uuid_generate_time(uuid_t out)
{
	UUID		g;
	RPC_STATUS	status = UuidCreateSequential(&g);

	/*
	 * RPC_S_UUID_LOCAL_ONLY: the machine has no network address, so the
	 * UUID is unique only on this machine
	 */
	if (status != RPC_S_OK && status != RPC_S_UUID_LOCAL_ONLY)
		abort();
	from_guid(&g, out);
}

void
uuid_unparse(const uuid_t uu, char *out)
{
	snprintf(out, 37,
			 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			 uu[0], uu[1], uu[2], uu[3], uu[4], uu[5], uu[6], uu[7],
			 uu[8], uu[9], uu[10], uu[11], uu[12], uu[13], uu[14], uu[15]);
}
