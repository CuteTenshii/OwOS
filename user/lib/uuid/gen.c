/* gen.c: uuid_generate_v4 and uuid_generate_v7, the parts of libuuid that read the entropy pool and the clock. */
#include "uuid.h"
#include "owos.h"

void uuid_generate_v4(uuid_t out)
{
  unsigned char r[16];
  rng_bytes(r, sizeof r);
  uuid_v4_from(out, r);
}

void uuid_generate_v7(uuid_t out)
{
  unsigned char r[10];
  rng_bytes(r, sizeof r);
  uuid_v7_from(out, (unsigned long long)now_ms(), r);
}
