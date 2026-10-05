/* PostGIS serialized geometry (version 2), read in place: POINT or LINESTRING with Z and M. Shared by the extension's
 * functions (laplace_pg.c) and its 4D index classes (gist4d.c). */
#ifndef LAPLACE_PG_GEO_H
#define LAPLACE_PG_GEO_H

#include "postgres.h"
#include "fmgr.h"
#include "varatt.h"

#define G2_Z 0x01
#define G2_M 0x02
#define G2_BBOX 0x04
#define G2_GEODETIC 0x08
#define G2_EXTENDED 0x10
#define G2_VERSION 0x40

typedef struct { uint32 type; uint32 n; const double *xyzm; } Geo;   /* POINT or LINESTRING with Z and M */

static inline Geo geo_of(Datum d){
    bytea *g = (bytea *)PG_DETOAST_DATUM(d); const uint8 *p = (const uint8 *)g; Geo r;
    uint8 flags = p[7];
    if (!(flags & G2_VERSION)) ereport(ERROR, (errmsg("laplace: expected a PostGIS geometry in serialization version 2")));
    if (!(flags & G2_Z) || !(flags & G2_M)) ereport(ERROR, (errmsg("laplace: expected a geometry with Z and M")));
    size_t off = 8;
    if (flags & G2_EXTENDED) off += 8;
    if (flags & G2_BBOX) off += (flags & G2_GEODETIC) ? 6 * sizeof(float) : 2 * 4 * sizeof(float);
    memcpy(&r.type, p + off, 4); memcpy(&r.n, p + off + 4, 4);
    if (r.type != 1 && r.type != 2) ereport(ERROR, (errmsg("laplace: expected a POINT or LINESTRING, got type %u", r.type)));
    r.xyzm = (const double *)(p + off + 8);
    if (off + 8 + (size_t)r.n * 32 > (size_t)VARSIZE(g)) ereport(ERROR, (errmsg("laplace: truncated geometry")));
    return r;
}

#endif
