/* High-level zero-SPI Native read operators retiring C# inline SQL CTE pyramids:
 *   - laplace_response_field: multi-round balanced per-occurrence coupling field
 *   - laplace_leaderboard:    consensus_standing + governed relation-band leaders
 *   - laplace_browse_entities: indexed entity browsing (prefix / entity_mask GIN / standing)
 */
#include "pg_core.h"
#include "geo.h"
#include "access/genam.h"
#include "access/gin.h"
#include "access/gin_private.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "nodes/tidbitmap.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include <ctype.h>
#include <math.h>

/* ---------------------------------------------------------------- shared utilities */

static inline double standing_confidence(double rating, double deviation)
{
    lp_rating r = { rating, deviation, LP_GLICKO_VOLATILITY };
    return lp_confidence(&r, 2.0);
}

static inline Datum lp_id_bytea_datum(const lp_id *id)
{
    return PointerGetDatum(bytes_datum(id->b, sizeof(lp_id)));
}

static lp_id *bytea_array_to_ids(ArrayType *arr, const char *fn_name, int *out_n)
{
    if (ARR_NDIM(arr) > 1)
        ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                        errmsg("%s: expected 1-D bytea[]", fn_name)));
    Datum *elems = NULL;
    bool *nulls = NULL;
    int n = 0;
    deconstruct_array(arr, BYTEAOID, -1, false, TYPALIGN_INT, &elems, &nulls, &n);
    lp_id *ids = palloc(sizeof(lp_id) * (Size)(n > 0 ? n : 1));
    for (int i = 0; i < n; i++) {
        if (nulls[i]) {
            pfree(ids); pfree(elems); pfree(nulls);
            ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                            errmsg("%s: seed IDs cannot be null", fn_name)));
        }
        bytea *b = DatumGetByteaPP(elems[i]);
        if (VARSIZE_ANY_EXHDR(b) != sizeof(lp_id)) {
            size_t got = (size_t)VARSIZE_ANY_EXHDR(b);
            pfree(ids); pfree(elems); pfree(nulls);
            ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                            errmsg("%s: each ID must be 16 bytes (got %zu)", fn_name, got)));
        }
        memcpy(ids[i].b, VARDATA_ANY(b), sizeof(lp_id));
    }
    if (elems) pfree(elems);
    if (nulls) pfree(nulls);
    *out_n = n;
    return ids;
}

static ArrayType *int32_vec_to_array(const int32 *vals, size_t n)
{
    if (n == 0) return construct_empty_array(INT4OID);
    Datum *d = palloc(sizeof(Datum) * n);
    for (size_t i = 0; i < n; i++) d[i] = Int32GetDatum(vals[i]);
    ArrayType *a = construct_array(d, (int)n, INT4OID, sizeof(int32), true, TYPALIGN_INT);
    pfree(d);
    return a;
}

static ArrayType *lp_id_vec_to_bytea_array(const lp_id *ids, size_t n)
{
    if (n == 0) return construct_empty_array(BYTEAOID);
    Datum *d = palloc(sizeof(Datum) * n);
    for (size_t i = 0; i < n; i++) d[i] = lp_id_bytea_datum(&ids[i]);
    ArrayType *a = construct_array(d, (int)n, BYTEAOID, -1, false, TYPALIGN_INT);
    for (size_t i = 0; i < n; i++) pfree(DatumGetPointer(d[i]));
    pfree(d);
    return a;
}

static void json_append_escaped(StringInfo out, const char *s, int len)
{
    appendStringInfoChar(out, '"');
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  appendStringInfoString(out, "\\\""); break;
            case '\\': appendStringInfoString(out, "\\\\"); break;
            case '\b': appendStringInfoString(out, "\\b"); break;
            case '\f': appendStringInfoString(out, "\\f"); break;
            case '\n': appendStringInfoString(out, "\\n"); break;
            case '\r': appendStringInfoString(out, "\\r"); break;
            case '\t': appendStringInfoString(out, "\\t"); break;
            default:
                if (c < 0x20u)
                    appendStringInfo(out, "\\u%04x", (unsigned)c);
                else
                    appendStringInfoChar(out, (char)c);
                break;
        }
    }
    appendStringInfoChar(out, '"');
}

/* ---------------------------------------------------------------- consensus_standing 16-way merge */

typedef struct {
    lp_id claim;
    double rating;
    double deviation;
    double volatility;
    double confidence;
    int64 matches;
} StandingRow;
typedef lp_vec(StandingRow) StandingRowVec;

static int cmp_standing_row_desc(const void *a, const void *b)
{
    const StandingRow *x = a, *y = b;
    bool nx = isnan(x->confidence), ny = isnan(y->confidence);
    if (nx != ny) return nx ? 1 : -1;
    if (x->confidence != y->confidence) return x->confidence > y->confidence ? -1 : 1;
    if (x->matches != y->matches) return x->matches > y->matches ? -1 : 1;
    return memcmp(x->claim.b, y->claim.b, sizeof(lp_id));
}

typedef struct {
    Relation part;
    Relation idx;
    TupleTableSlot *slot;
    IndexScanDesc iscan;
    AttrNumber a_claim, a_rating, a_dev, a_vol, a_matches;
    StandingRow *fallback_rows;
    size_t fallback_n;
    size_t fallback_pos;
    bool use_fallback;
    StandingRow cur;
    bool active;
} StandingPartCursor;

typedef struct {
    StandingPartCursor *parts;
    int nparts;
    int64 min_matches;
    MemoryContext ctx;
} StandingMerge;

static Oid find_consensus_standing_index(Relation part, AttrNumber a_claim, AttrNumber a_matches)
{
    List *idxs = RelationGetIndexList(part);
    ListCell *lc;
    Oid found = InvalidOid;
    foreach(lc, idxs) {
        Oid ioid = lfirst_oid(lc);
        Relation idx = index_open(ioid, AccessShareLock);
        if (idx->rd_rel->relam == BTREE_AM_OID && idx->rd_index &&
            idx->rd_index->indisvalid && idx->rd_index->indisready && idx->rd_index->indislive &&
            !idx->rd_index->indisunique &&
            idx->rd_index->indnkeyatts == 3 &&
            idx->rd_index->indkey.values[0] == 0 &&
            idx->rd_index->indkey.values[1] == a_matches &&
            idx->rd_index->indkey.values[2] == a_claim &&
            !heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL)) {
            found = ioid;
            index_close(idx, NoLock);
            break;
        }
        index_close(idx, NoLock);
    }
    list_free(idxs);
    return found;
}

static bool standing_read_slot(TupleTableSlot *slot, AttrNumber ac, AttrNumber ar,
                               AttrNumber ad, AttrNumber av, AttrNumber am,
                               int64 min_matches, StandingRow *out)
{
    bool nc = true, nr = true, nd = true, nv = true, nm = true;
    Datum dm = slot_getattr(slot, am, &nm);
    if (nm) return false;
    int64 matches = DatumGetInt64(dm);
    if (matches < min_matches) return false;
    Datum dc = slot_getattr(slot, ac, &nc);
    Datum dr = slot_getattr(slot, ar, &nr);
    Datum dd = slot_getattr(slot, ad, &nd);
    Datum dv = slot_getattr(slot, av, &nv);
    if (nc || nr || nd || nv) return false;
    memcpy(out->claim.b, DatumGetPointer(dc), sizeof(lp_id));
    out->rating = DatumGetFloat8(dr);
    out->deviation = DatumGetFloat8(dd);
    out->volatility = DatumGetFloat8(dv);
    out->confidence = standing_confidence(out->rating, out->deviation);
    out->matches = matches;
    return true;
}

static bool standing_part_advance(StandingPartCursor *pc, int64 min_matches)
{
    if (pc->use_fallback) {
        if (pc->fallback_pos < pc->fallback_n) {
            pc->cur = pc->fallback_rows[pc->fallback_pos++];
            pc->active = true;
            return true;
        }
        pc->active = false;
        return false;
    }
    while (index_getnext_slot(pc->iscan, ForwardScanDirection, pc->slot)) {
        CHECK_FOR_INTERRUPTS();
        bool ok = standing_read_slot(pc->slot, pc->a_claim, pc->a_rating, pc->a_dev,
                                     pc->a_vol, pc->a_matches, min_matches, &pc->cur);
        ExecClearTuple(pc->slot);
        if (ok) {
            pc->active = true;
            return true;
        }
    }
    pc->active = false;
    return false;
}

static void standing_merge_open(StandingMerge *m, int64 min_matches)
{
    memset(m, 0, sizeof(*m));
    m->min_matches = min_matches;
    m->ctx = AllocSetContextCreate(CurrentMemoryContext, "StandingMerge", ALLOCSET_DEFAULT_SIZES);

    Oid target = lp_pg_relation("consensus");
    const char *selected[] = { "claim", "rating", "deviation", "volatility", "matches" };
    const char *column = NULL;
    Relation parent = lp_pg_read_parent(target, &column, selected, 5);
    table_close(parent, NoLock);

    const Oid *parts = NULL;
    int nparts = lp_pg_canonical_partitions("consensus", &parts, NULL, NULL, NULL);
    m->nparts = nparts;
    m->parts = MemoryContextAllocZero(m->ctx, (Size)Max(nparts, 1) * sizeof(StandingPartCursor));
    Snapshot snap = GetActiveSnapshot();

    for (int p = 0; p < nparts; p++) {
        CHECK_FOR_INTERRUPTS();
        StandingPartCursor *pc = &m->parts[p];
        pc->part = table_open(parts[p], AccessShareLock);
        pc->a_claim = get_attnum(parts[p], "claim");
        pc->a_rating = get_attnum(parts[p], "rating");
        pc->a_dev = get_attnum(parts[p], "deviation");
        pc->a_vol = get_attnum(parts[p], "volatility");
        pc->a_matches = get_attnum(parts[p], "matches");

        Oid idx_oid = (min_matches >= 1) ? find_consensus_standing_index(pc->part, pc->a_claim, pc->a_matches) : InvalidOid;
        if (OidIsValid(idx_oid)) {
            pc->idx = index_open(idx_oid, AccessShareLock);
            pc->slot = table_slot_create(pc->part, NULL);
            pc->iscan = index_beginscan(pc->part, pc->idx, snap, NULL, 0, 0);
            index_rescan(pc->iscan, NULL, 0, NULL, 0);
            (void)standing_part_advance(pc, min_matches);
        } else {
            pc->use_fallback = true;
            lp_vec(StandingRow) rows = { 0 };
            TableScanDesc tscan = table_beginscan(pc->part, snap, 0, NULL);
            TupleTableSlot *slot = table_slot_create(pc->part, NULL);
            while (table_scan_getnextslot(tscan, ForwardScanDirection, slot)) {
                CHECK_FOR_INTERRUPTS();
                StandingRow sr;
                if (standing_read_slot(slot, pc->a_claim, pc->a_rating, pc->a_dev,
                                       pc->a_vol, pc->a_matches, min_matches, &sr))
                    lp_push(&rows, sr);
                ExecClearTuple(slot);
            }
            ExecDropSingleTupleTableSlot(slot);
            table_endscan(tscan);
            if (rows.n > 1)
                lp_sort(rows.v, rows.n, sizeof(StandingRow), cmp_standing_row_desc);
            if (rows.n > 0) {
                pc->fallback_rows = MemoryContextAlloc(m->ctx, rows.n * sizeof(StandingRow));
                memcpy(pc->fallback_rows, rows.v, rows.n * sizeof(StandingRow));
                pc->fallback_n = rows.n;
            }
            lp_vec_free(&rows);
            (void)standing_part_advance(pc, min_matches);
        }
    }
}

static bool standing_merge_next(StandingMerge *m, StandingRow *out)
{
    int best = -1;
    for (int p = 0; p < m->nparts; p++) {
        if (!m->parts[p].active) continue;
        if (best < 0 || cmp_standing_row_desc(&m->parts[p].cur, &m->parts[best].cur) < 0)
            best = p;
    }
    if (best < 0) return false;
    *out = m->parts[best].cur;
    (void)standing_part_advance(&m->parts[best], m->min_matches);
    return true;
}

static void standing_merge_close(StandingMerge *m)
{
    if (m->parts) {
        for (int p = 0; p < m->nparts; p++) {
            StandingPartCursor *pc = &m->parts[p];
            if (pc->iscan) index_endscan(pc->iscan);
            if (pc->slot) ExecDropSingleTupleTableSlot(pc->slot);
            if (pc->idx) index_close(pc->idx, NoLock);
            if (pc->part) table_close(pc->part, NoLock);
        }
        m->parts = NULL;
    }
    if (m->ctx) {
        MemoryContextDelete(m->ctx);
        m->ctx = NULL;
    }
}

/* ---------------------------------------------------------------- 1. laplace_response_field */

typedef struct { double x[4]; bool has; } RfCoord4;
static void rf_coord_visit(int64 ordinal, const lp_id *id, int16 tier, Datum coord, int64 hilbert, void *context)
{
    (void)ordinal; (void)tier; (void)hilbert;
    if (!coord) return;
    RfCoord4 *c = lp_idmap_lookup((lp_idmap *)context, id);
    if (!c) return;
    Geo g = geo_of(coord);
    if (g.p.n >= 1) { memcpy(c->x, xyzm(&g), 32); c->has = true; }
}

static void rf_coords_load(lp_idmap *at)
{
    size_t n = lp_idmap_count(at);
    if (!n) return;
    const lp_tier0_record *floor = lp_pg_tier0();
    lp_id *rest = palloc(sizeof(lp_id) * n);
    size_t nr = 0;
    for (size_t i = 0; i < n; i++) {
        RfCoord4 *c = lp_idmap_at(at, i);
        const lp_tier0_record *atom = lp_tier0_find(floor, lp_idmap_key(at, i));
        if (atom) {
            lp_coord k;
            memcpy(k.m, atom->m, sizeof k.m);
            lp_coord_xyzm(&k, c->x);
            c->has = true;
        } else {
            rest[nr++] = *lp_idmap_key(at, i);
        }
    }
    if (nr) lp_pg_lookup_entity_coords(rest, nr, rf_coord_visit, at);
    pfree(rest);
}

typedef struct { lp_id id; lp_id *v; int nv; double d; } RfCurve;
typedef lp_vec(RfCurve) RfCurves;

static int cmp_rf_curve(const void *a, const void *b)
{
    double x = ((const RfCurve *)a)->d, y = ((const RfCurve *)b)->d;
    return x < y ? -1 : x > y ? 1 : memcmp(((const RfCurve *)a)->id.b, ((const RfCurve *)b)->id.b, sizeof(lp_id));
}

static void rf_nominate_curve(RfCurves *cv, lp_idmap *seen, const lp_id *id, Datum path, MemoryContext ctx)
{
    bool fresh = false;
    lp_idmap_put(seen, id, &fresh);
    if (!fresh) return;
    Geo g = geo_of(path);
    size_t n = lp_path_len(g.p);
    if (n < 2 || n > 1024) return;
    RfCurve *c = lp_vec_add(cv);
    c->id = *id;
    c->v = MemoryContextAlloc(ctx, sizeof(lp_id) * n);
    c->nv = (int)lp_path_expand(g.p, c->v, n);
    c->d = INFINITY;
}

typedef struct { RfCurves *cv; lp_idmap *seen; MemoryContext ctx; } RfKnnNom;
static void rf_knn_path(const lp_id *id, Datum path, Datum mask, void *context)
{
    (void)mask;
    RfKnnNom *k = context;
    if (path) rf_nominate_curve(k->cv, k->seen, id, path, k->ctx);
}

/* Compact sorted 1-based occurrence origin list (supports arbitrary seed counts). */
typedef struct {
    int32 *v;
    int n;
    int cap;
} RfOrigins;

static void rf_origins_add(RfOrigins *o, int32 occ, MemoryContext ctx)
{
    for (int i = 0; i < o->n; i++) {
        if (o->v[i] == occ) return;
    }
    if (o->n == o->cap) {
        int ncap = o->cap ? o->cap * 2 : 4;
        int32 *nv = MemoryContextAlloc(ctx, sizeof(int32) * (Size)ncap);
        if (o->n > 0) memcpy(nv, o->v, sizeof(int32) * (Size)o->n);
        o->v = nv;
        o->cap = ncap;
    }
    int pos = o->n;
    while (pos > 0 && o->v[pos - 1] > occ) {
        o->v[pos] = o->v[pos - 1];
        pos--;
    }
    o->v[pos] = occ;
    o->n++;
}

static bool rf_origins_has(const RfOrigins *o, int32 occ)
{
    for (int i = 0; i < o->n; i++) {
        if (o->v[i] == occ) return true;
        if (o->v[i] > occ) return false;
    }
    return false;
}

static void rf_origins_merge(RfOrigins *dst, const RfOrigins *src, MemoryContext ctx)
{
    for (int i = 0; i < src->n; i++) rf_origins_add(dst, src->v[i], ctx);
}

typedef struct {
    int32 round;
    int32 occ;
    bool has_source;
    lp_id source;
    lp_id target;
    int16 route;
    bool has_claim;
    lp_id claim;
    bool has_relation;
    lp_id relation;
    double rating;
    double deviation;
    double volatility;
    double confidence;
    int64 matches;
    int64 witnesses;
    int16 tier;
    bool has_distance;
    double distance;
    bool has_outbound;
    bool outbound;
    RfOrigins origins;
    lp_id *bindings;
    int nbindings;
    lp_id *vertices;
    int nvertices;
} RfRow;

typedef lp_vec(RfRow) RfRows;

typedef struct {
    int seed_idx;
    lp_id claim;
    lp_id target;
    lp_id relation;
    double rating;
    double deviation;
    double volatility;
    double confidence;
    int64 matches;
    bool outbound;
} RfRawStrand;

typedef struct {
    const lp_id *anchors;
    int nanchors;
    lp_vec(RfRawStrand) hits;
} RfStrandCollect;

static void rf_strand_hit(const PgClaimHit *hit, void *context)
{
    RfStrandCollect *c = context;
    int idx = (int)hit->i - 1;
    if (idx < 0 || idx >= c->nanchors || !hit->path) return;
    Geo g = geo_of(hit->path);
    lp_id part[64];
    size_t np = lp_path_expand(g.p, part, 64);
    if (np < 2 || np > 64) return;
    int other = lp_tuple_other(part, np, &c->anchors[idx]);
    if (other < 0) return;
    RfRawStrand *s = lp_vec_add(&c->hits);
    s->seed_idx = idx;
    s->claim = hit->entity;
    s->target = part[other];
    s->relation = (np >= 3) ? part[1] : c->anchors[idx];
    s->rating = hit->rating;
    s->deviation = hit->deviation;
    s->volatility = hit->volatility;
    s->confidence = standing_confidence(hit->rating, hit->deviation);
    s->matches = hit->matches;
    s->outbound = lp_id_eq(&part[0], &c->anchors[idx]);
}

static int cmp_rf_raw_strand(const void *a, const void *b)
{
    const RfRawStrand *x = a, *y = b;
    if (x->confidence != y->confidence) return x->confidence > y->confidence ? -1 : 1;
    if (x->matches != y->matches) return x->matches > y->matches ? -1 : 1;
    int c = memcmp(x->claim.b, y->claim.b, sizeof(lp_id));
    if (c) return c;
    return memcmp(x->target.b, y->target.b, sizeof(lp_id));
}

typedef struct {
    RfRows *rows;
    const lp_id *seeds;
    const int *orig_idx;
    int group_n;
    int *occ_counts;
    int keep;
    int nearest;
    RfCurves *cv;
    lp_idmap *seen_curves;
    MemoryContext ctx;
} RfContainCollect;

static void rf_contain_hit(const PgHolderHit *hit, void *context)
{
    RfContainCollect *c = context;
    if (hit->hub || !hit->path) return;
    int64 gidx = hit->ordinal - 1;
    if (gidx < 0 || gidx >= c->group_n) return;
    int orig = c->orig_idx[gidx];
    if (c->occ_counts[orig] < c->keep) {
        c->occ_counts[orig]++;
        RfRow *r = lp_vec_add(c->rows);
        r->round = 1;
        r->occ = orig + 1;
        r->has_source = true;
        r->source = c->seeds[orig];
        r->target = hit->entity;
        r->route = 1;
        r->tier = hit->tier;
        rf_origins_add(&r->origins, orig + 1, c->ctx);
    }
    if (c->nearest > 0)
        rf_nominate_curve(c->cv, c->seen_curves, &hit->entity, hit->path, c->ctx);
}

typedef struct {
    lp_id id;
    double best_conf;
    int64 best_matches;
} RfSeedCand;

static int cmp_rf_seed_cand(const void *a, const void *b)
{
    const RfSeedCand *x = a, *y = b;
    if (x->best_conf != y->best_conf) return x->best_conf > y->best_conf ? -1 : 1;
    if (x->best_matches != y->best_matches) return x->best_matches > y->best_matches ? -1 : 1;
    return memcmp(x->id.b, y->id.b, sizeof(lp_id));
}

typedef struct {
    lp_id id;
    int32 primary_occ;
    RfOrigins origins;
    lp_vec(lp_id) bindings;
} RfRoundSeed;

typedef struct {
    lp_id claim;
    lp_id trunk;
} RfAttPair;

static int cmp_rf_att_pair(const void *a, const void *b)
{
    const RfAttPair *x = a, *y = b;
    int c = memcmp(x->claim.b, y->claim.b, sizeof(lp_id));
    return c ? c : memcmp(x->trunk.b, y->trunk.b, sizeof(lp_id));
}

PG_FUNCTION_INFO_V1(laplace_response_field);
Datum laplace_response_field(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_response_field", &td);
    if (PG_ARGISNULL(0)) PG_RETURN_NULL();

    int32 keep = (PG_NARGS() > 1 && !PG_ARGISNULL(1)) ? PG_GETARG_INT32(1) : 32;
    int32 max_rounds = (PG_NARGS() > 2 && !PG_ARGISNULL(2)) ? PG_GETARG_INT32(2) : 2;
    int32 nearest = (PG_NARGS() > 3 && !PG_ARGISNULL(3)) ? PG_GETARG_INT32(3) : 0;
    int32 max_text = (PG_NARGS() > 4 && !PG_ARGISNULL(4)) ? PG_GETARG_INT32(4) : 160;

    if (keep < 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_response_field: keep must be >= 1 (got %d)", keep)));
    if (max_rounds < 1 || max_rounds > 4)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_response_field: max_rounds must be between 1 and 4 (got %d)", max_rounds)));
    if (nearest < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_response_field: nearest must be >= 0 (got %d)", nearest)));
    if (max_text < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_response_field: max_text must be >= 0 (got %d)", max_text)));

    int nocc = 0;
    lp_id *seeds = bytea_array_to_ids(PG_GETARG_ARRAYTYPE_P(0), "laplace_response_field", &nocc);
    if (nocc <= 0) { pfree(seeds); PG_RETURN_NULL(); }

    MemoryContext caller = CurrentMemoryContext;
    MemoryContext rfctx = AllocSetContextCreate(caller, "laplace_response_field", ALLOCSET_DEFAULT_SIZES);
    RfRows rows = { 0 };
    RfCurves cv = { 0 };
    lp_idmap *seen_curves = lp_idmap_new();
    lp_idmap *expanded_anchors = lp_idmap_new();
    lp_idmap *entity_origins = lp_idmap_sized(sizeof(RfOrigins));
    lp_idmap *witness_counts = lp_idmap_sized(sizeof(int64));
    lp_idmap *label_map = lp_idmap_sized(sizeof(PgLabelEntry));

    PG_TRY();
    {
        for (int i = 0; i < nocc; i++)
            lp_idmap_put(expanded_anchors, &seeds[i], NULL);

        /* ---- Round 1: strands (route 0) ---- */
        int64 r1_fan = Max((int64)keep * 2, (int64)64);
        RfStrandCollect sc1 = { .anchors = seeds, .nanchors = nocc };
        lp_pg_claims_each_direct(seeds, nocc, r1_fan, UINT64_C(1),
                                 NULL, 0, 0, NULL, 0, 0, NULL, 0,
                                 rf_strand_hit, &sc1);
        if (sc1.hits.n > 0) {
            typedef lp_vec(RfRawStrand) StrandBucket;
            StrandBucket *buckets = palloc0(sizeof(StrandBucket) * (Size)nocc);
            for (size_t i = 0; i < sc1.hits.n; i++) {
                int idx = sc1.hits.v[i].seed_idx;
                if (idx >= 0 && idx < nocc) lp_push(&buckets[idx], sc1.hits.v[i]);
            }
            lp_idmap *dedup = lp_idmap_new();
            for (int o = 0; o < nocc; o++) {
                if (buckets[o].n == 0) continue;
                if (buckets[o].n > 1)
                    lp_sort(buckets[o].v, buckets[o].n, sizeof(RfRawStrand), cmp_rf_raw_strand);
                lp_idmap_clear(dedup);
                int kept = 0;
                for (size_t i = 0; i < buckets[o].n && kept < keep; i++) {
                    const RfRawStrand *s = &buckets[o].v[i];
                    bool fresh = false;
                    lp_idmap_put(dedup, &s->claim, &fresh);
                    if (!fresh) continue;
                    RfRow *r = lp_vec_add(&rows);
                    r->round = 1;
                    r->occ = o + 1;
                    r->has_source = true;
                    r->source = seeds[o];
                    r->target = s->target;
                    r->route = 0;
                    r->has_claim = true;
                    r->claim = s->claim;
                    r->has_relation = true;
                    r->relation = s->relation;
                    r->rating = s->rating;
                    r->deviation = s->deviation;
                    r->volatility = s->volatility;
                    r->confidence = s->confidence;
                    r->matches = s->matches;
                    r->witnesses = s->matches;
                    r->tier = 0;
                    r->has_outbound = true;
                    r->outbound = s->outbound;
                    rf_origins_add(&r->origins, o + 1, rfctx);
                    r->bindings = MemoryContextAlloc(rfctx, sizeof(lp_id));
                    r->bindings[0] = s->claim;
                    r->nbindings = 1;
                    kept++;
                }
                lp_vec_free(&buckets[o]);
            }
            lp_idmap_free(dedup);
            pfree(buckets);
        }
        lp_vec_free(&sc1.hits);

        /* ---- Round 1: containment (route 1) ---- */
        {
            int16 *occ_tier = palloc(sizeof(int16) * (Size)nocc);
            lp_pg_lookup_entity_tiers(seeds, (size_t)nocc, occ_tier);
            bool *done = palloc0(sizeof(bool) * (Size)nocc);
            lp_id *g_ids = palloc(sizeof(lp_id) * (Size)nocc);
            int *g_orig = palloc(sizeof(int) * (Size)nocc);
            int *occ_counts = palloc0(sizeof(int) * (Size)nocc);
            for (int i = 0; i < nocc; i++) {
                if (done[i] || occ_tier[i] < 0) continue;
                int16 t = occ_tier[i];
                int gn = 0;
                for (int j = i; j < nocc; j++) {
                    if (!done[j] && occ_tier[j] == t) {
                        done[j] = true;
                        g_ids[gn] = seeds[j];
                        g_orig[gn] = j;
                        gn++;
                    }
                }
                RfContainCollect cc = { &rows, seeds, g_orig, gn, occ_counts, keep, nearest, &cv, seen_curves, rfctx };
                lp_pg_holders_direct(g_ids, gn, NULL, 0, true, t, keep, 2, 0, 0, rf_contain_hit, &cc);
            }
            pfree(occ_counts); pfree(g_orig); pfree(g_ids); pfree(done); pfree(occ_tier);
        }

        /* ---- Round 1: shape curves (route 2, when nearest > 0) ---- */
        if (nearest > 0 && nocc >= 2) {
            PgSalience *sal = palloc(sizeof(PgSalience) * (Size)nocc);
            lp_pg_salience(seeds, nocc, NULL, 0, sal);
            lp_idmap *oc = lp_idmap_sized(sizeof(RfCoord4));
            for (int i = 0; i < nocc; i++) lp_idmap_put(oc, &seeds[i], NULL);
            rf_coords_load(oc);
            double *pa = palloc(sizeof(double) * 4 * (Size)nocc);
            double cen[4] = { 0 }, wsum = 0.0;
            int npa = 0;
            for (int i = 0; i < nocc; i++) {
                const RfCoord4 *c = lp_idmap_lookup(oc, &seeds[i]);
                if (c && c->has) {
                    memcpy(pa + 4 * npa++, c->x, 32);
                    double w = sal[i].salience;
                    if (w > 0.0) {
                        for (int k = 0; k < 4; k++) cen[k] += w * c->x[k];
                        wsum += w;
                    }
                }
            }
            lp_idmap_free(oc);
            pfree(sal);
            if (npa >= 2) {
                if (wsum > 0.0) {
                    for (int k = 0; k < 4; k++) cen[k] /= wsum;
                } else {
                    for (int i = 0; i < npa; i++)
                        for (int k = 0; k < 4; k++) cen[k] += pa[4 * i + k] / (double)npa;
                }
                int knn_k = Max(keep, nearest);
                PgKnnHit *khits = palloc(sizeof(PgKnnHit) * (Size)knn_k);
                int nk = lp_pg_knn4d(cen, knn_k, false, 3, false, 0, NULL, khits, CurrentMemoryContext);
                if (nk > 0) {
                    lp_id *kids = palloc(sizeof(lp_id) * (Size)nk);
                    for (int i = 0; i < nk; i++) kids[i] = khits[i].id;
                    RfKnnNom kn = { &cv, seen_curves, rfctx };
                    lp_pg_lookup_paths(kids, (size_t)nk, false, rf_knn_path, &kn);
                    pfree(kids);
                }
                pfree(khits);

                if (cv.n > 0) {
                    lp_idmap *kc = lp_idmap_sized(sizeof(RfCoord4));
                    for (size_t i = 0; i < cv.n; i++)
                        for (int k = 0; k < cv.v[i].nv; k++) lp_idmap_put(kc, &cv.v[i].v[k], NULL);
                    rf_coords_load(kc);
                    double *b = palloc(sizeof(double) * 4 * 1024);
                    for (size_t i = 0; i < cv.n; i++) {
                        int nb = 0;
                        for (int k = 0; k < cv.v[i].nv; k++) {
                            const RfCoord4 *c = lp_idmap_lookup(kc, &cv.v[i].v[k]);
                            if (c && c->has) memcpy(b + 4 * nb++, c->x, 32);
                        }
                        if (nb >= 2) {
                            size_t steps = 0;
                            double d = lp_dtw4(pa, (size_t)npa, b, (size_t)nb, &steps);
                            cv.v[i].d = steps ? d / (double)steps : d;
                        }
                    }
                    pfree(b);
                    lp_idmap_free(kc);
                    if (cv.n > 1) lp_sort(cv.v, cv.n, sizeof(RfCurve), cmp_rf_curve);
                    int emitted_shape = 0;
                    for (size_t i = 0; i < cv.n && emitted_shape < nearest; i++) {
                        if (!isfinite(cv.v[i].d)) continue;
                        RfRow *r = lp_vec_add(&rows);
                        r->round = 1;
                        r->occ = 0;
                        r->has_source = false;
                        r->target = cv.v[i].id;
                        r->route = 2;
                        r->tier = 0;
                        r->has_distance = true;
                        r->distance = cv.v[i].d;
                        r->vertices = cv.v[i].v;
                        r->nvertices = cv.v[i].nv;
                        emitted_shape++;
                    }
                }
            }
            pfree(pa);
        }

        /* ---- Rounds 2..max_rounds: strictly balanced per occurrence ---- */
        int per_occ_seed_cap = Max(4, (keep + nocc - 1) / nocc);
        if (per_occ_seed_cap > keep) per_occ_seed_cap = keep;
        int per_occ_row_cap = Max(keep * 4, 64);

        for (int32 rnd = 2; rnd <= max_rounds; rnd++) {
            CHECK_FOR_INTERRUPTS();
            /* Collect candidate seeds per input occurrence from round (rnd - 1). */
            lp_idmap **occ_cands = palloc(sizeof(lp_idmap *) * (Size)nocc);
            for (int o = 0; o < nocc; o++) occ_cands[o] = lp_idmap_sized(sizeof(RfSeedCand));

            /* Track full origins & binding claims for every entity reached in rnd - 1. */
            lp_idmap *prev_origins = lp_idmap_sized(sizeof(RfOrigins));
            typedef lp_vec(lp_id) BindVec;
            lp_idmap *prev_binds = lp_idmap_sized(sizeof(BindVec));

            for (size_t i = 0; i < rows.n; i++) {
                const RfRow *r = &rows.v[i];
                if (r->round != rnd - 1 || r->route != 0) continue;
                if (lp_idmap_find(expanded_anchors, &r->target) >= 0) continue;

                bool fo = false;
                RfOrigins *po = lp_idmap_get(prev_origins, &r->target, &fo);
                if (fo) memset(po, 0, sizeof(*po));
                rf_origins_merge(po, &r->origins, rfctx);

                if (r->nbindings > 0) {
                    bool fb = false;
                    BindVec *bv = lp_idmap_get(prev_binds, &r->target, &fb);
                    if (fb) memset(bv, 0, sizeof(*bv));
                    for (int b = 0; b < r->nbindings && bv->n < 16; b++) {
                        bool dup = false;
                        for (size_t k = 0; k < bv->n; k++)
                            if (lp_id_eq(&bv->v[k], &r->bindings[b])) { dup = true; break; }
                        if (!dup) lp_push(bv, r->bindings[b]);
                    }
                }

                for (int oi = 0; oi < r->origins.n; oi++) {
                    int o = r->origins.v[oi] - 1;
                    if (o < 0 || o >= nocc) continue;
                    bool fresh = false;
                    RfSeedCand *cand = lp_idmap_get(occ_cands[o], &r->target, &fresh);
                    if (fresh || r->confidence > cand->best_conf ||
                        (r->confidence == cand->best_conf && r->matches > cand->best_matches)) {
                        cand->id = r->target;
                        cand->best_conf = r->confidence;
                        cand->best_matches = r->matches;
                    }
                }
            }

            /* Balanced selection: each occurrence o picks up to per_occ_seed_cap seeds. */
            lp_vec(RfRoundSeed) rseeds = { 0 };
            lp_idmap *rseed_idx = lp_idmap_sized(sizeof(size_t));
            for (int o = 0; o < nocc; o++) {
                size_t nc = lp_idmap_count(occ_cands[o]);
                if (nc == 0) continue;
                RfSeedCand *arr = palloc(sizeof(RfSeedCand) * nc);
                for (size_t k = 0; k < nc; k++) arr[k] = *(RfSeedCand *)lp_idmap_at(occ_cands[o], k);
                if (nc > 1) lp_sort(arr, nc, sizeof(RfSeedCand), cmp_rf_seed_cand);
                size_t take = Min(nc, (size_t)per_occ_seed_cap);
                for (size_t k = 0; k < take; k++) {
                    bool fresh = false;
                    size_t *slot = lp_idmap_get(rseed_idx, &arr[k].id, &fresh);
                    if (fresh) {
                        *slot = rseeds.n;
                        RfRoundSeed *rs = lp_vec_add(&rseeds);
                        rs->id = arr[k].id;
                        rs->primary_occ = o + 1;
                        const RfOrigins *po = lp_idmap_lookup(prev_origins, &rs->id);
                        if (po) rf_origins_merge(&rs->origins, po, rfctx);
                        else rf_origins_add(&rs->origins, o + 1, rfctx);
                        const BindVec *bv = lp_idmap_lookup(prev_binds, &rs->id);
                        if (bv) {
                            for (size_t b = 0; b < bv->n; b++) lp_push(&rs->bindings, bv->v[b]);
                        }
                        lp_idmap_put(expanded_anchors, &rs->id, NULL);
                    }
                }
                pfree(arr);
            }

            for (int o = 0; o < nocc; o++) lp_idmap_free(occ_cands[o]);
            pfree(occ_cands);
            for (size_t i = 0; i < lp_idmap_count(prev_binds); i++)
                lp_vec_free((BindVec *)lp_idmap_at(prev_binds, i));
            lp_idmap_free(prev_binds);
            lp_idmap_free(prev_origins);
            lp_idmap_free(rseed_idx);

            if (rseeds.n == 0) {
                lp_vec_free(&rseeds);
                break;
            }

            lp_id *rseed_ids = palloc(sizeof(lp_id) * rseeds.n);
            for (size_t i = 0; i < rseeds.n; i++) rseed_ids[i] = rseeds.v[i].id;

            int64 r_fan = Max((int64)keep, (int64)12);
            RfStrandCollect scr = { .anchors = rseed_ids, .nanchors = (int)rseeds.n };
            lp_pg_claims_each_direct(rseed_ids, (int)rseeds.n, r_fan, UINT64_C(1),
                                     NULL, 0, 0, NULL, 0, 0, NULL, 0,
                                     rf_strand_hit, &scr);

            if (scr.hits.n > 0) {
                typedef lp_vec(RfRawStrand) StrandBucket;
                StrandBucket *occ_hits = palloc0(sizeof(StrandBucket) * (Size)nocc);
                for (size_t i = 0; i < scr.hits.n; i++) {
                    int sidx = scr.hits.v[i].seed_idx;
                    if (sidx < 0 || (size_t)sidx >= rseeds.n) continue;
                    int po = rseeds.v[sidx].primary_occ - 1;
                    if (po >= 0 && po < nocc) lp_push(&occ_hits[po], scr.hits.v[i]);
                }
                lp_idmap *dedup = lp_idmap_new();
                for (int o = 0; o < nocc; o++) {
                    if (occ_hits[o].n == 0) continue;
                    if (occ_hits[o].n > 1)
                        lp_sort(occ_hits[o].v, occ_hits[o].n, sizeof(RfRawStrand), cmp_rf_raw_strand);
                    lp_idmap_clear(dedup);
                    int kept = 0;
                    for (size_t i = 0; i < occ_hits[o].n && kept < per_occ_row_cap; i++) {
                        const RfRawStrand *s = &occ_hits[o].v[i];
                        bool fresh = false;
                        lp_idmap_put(dedup, &s->claim, &fresh);
                        if (!fresh) continue;
                        const RfRoundSeed *rs = &rseeds.v[s->seed_idx];
                        RfRow *r = lp_vec_add(&rows);
                        r->round = rnd;
                        r->occ = rs->primary_occ;
                        r->has_source = true;
                        r->source = rs->id;
                        r->target = s->target;
                        r->route = 0;
                        r->has_claim = true;
                        r->claim = s->claim;
                        r->has_relation = true;
                        r->relation = s->relation;
                        r->rating = s->rating;
                        r->deviation = s->deviation;
                        r->volatility = s->volatility;
                        r->confidence = s->confidence;
                        r->matches = s->matches;
                        r->witnesses = s->matches;
                        r->tier = 0;
                        r->has_outbound = true;
                        r->outbound = s->outbound;
                        rf_origins_merge(&r->origins, &rs->origins, rfctx);
                        size_t nb = rs->bindings.n;
                        bool has_self = false;
                        for (size_t b = 0; b < nb; b++)
                            if (lp_id_eq(&rs->bindings.v[b], &s->claim)) { has_self = true; break; }
                        size_t tot_b = nb + (has_self ? 0 : 1);
                        r->bindings = MemoryContextAlloc(rfctx, sizeof(lp_id) * tot_b);
                        if (nb > 0) memcpy(r->bindings, rs->bindings.v, sizeof(lp_id) * nb);
                        if (!has_self) r->bindings[nb] = s->claim;
                        r->nbindings = (int)tot_b;
                        kept++;
                    }
                    lp_vec_free(&occ_hits[o]);
                }
                lp_idmap_free(dedup);
                pfree(occ_hits);
            }

            lp_vec_free(&scr.hits);
            pfree(rseed_ids);
            for (size_t i = 0; i < rseeds.n; i++) lp_vec_free(&rseeds.v[i].bindings);
            lp_vec_free(&rseeds);
        }

        /* ---- Batch witness counts from attestation PK ---- */
        if (rows.n > 0) {
            lp_idmap *claim_set = lp_idmap_new();
            for (size_t i = 0; i < rows.n; i++) {
                if (rows.v[i].has_claim) lp_idmap_put(claim_set, &rows.v[i].claim, NULL);
            }
            size_t nc = lp_idmap_count(claim_set);
            if (nc > 0) {
                PgAttrRows att = { 0 };
                lp_pg_read_attestation_pk(lp_idmap_keys(claim_set), (int)nc, NULL, &att, NULL);
                if (att.n > 0) {
                    RfAttPair *pairs = palloc(sizeof(RfAttPair) * att.n);
                    for (size_t i = 0; i < att.n; i++)
                        pairs[i] = (RfAttPair){ att.v[i].claim, att.v[i].trunk };
                    if (att.n > 1) lp_sort(pairs, att.n, sizeof(RfAttPair), cmp_rf_att_pair);
                    size_t i = 0;
                    while (i < att.n) {
                        int64 wcnt = 1;
                        size_t j = i + 1;
                        while (j < att.n && lp_id_eq(&pairs[j].claim, &pairs[i].claim)) {
                            if (!lp_id_eq(&pairs[j].trunk, &pairs[j - 1].trunk)) wcnt++;
                            j++;
                        }
                        *(int64 *)lp_idmap_get(witness_counts, &pairs[i].claim, NULL) = wcnt;
                        i = j;
                    }
                    pfree(pairs);
                }
                lp_vec_free(&att);
            }
            lp_idmap_free(claim_set);
        }

        /* ---- Build entity -> all origins map and collect all label IDs ---- */
        lp_idmap *label_ids = lp_idmap_new();
        for (size_t i = 0; i < rows.n; i++) {
            const RfRow *r = &rows.v[i];
            if (r->has_source) lp_idmap_put(label_ids, &r->source, NULL);
            lp_idmap_put(label_ids, &r->target, NULL);
            if (r->has_relation) lp_idmap_put(label_ids, &r->relation, NULL);

            if (r->origins.n > 0) {
                bool fresh = false;
                RfOrigins *eo = lp_idmap_get(entity_origins, &r->target, &fresh);
                if (fresh) memset(eo, 0, sizeof(*eo));
                rf_origins_merge(eo, &r->origins, rfctx);
            }
        }
        if (lp_idmap_count(label_ids) > 0) {
            lp_pg_realize_labels_map(lp_idmap_keys(label_ids), lp_idmap_count(label_ids),
                                     max_text, false, rfctx, label_map);
        }
        lp_idmap_free(label_ids);

        /* ---- Emit final typed rows ---- */
        for (size_t i = 0; i < rows.n; i++) {
            CHECK_FOR_INTERRUPTS();
            const RfRow *r = &rows.v[i];

            const PgLabelEntry *src_lbl = r->has_source ? lp_idmap_lookup(label_map, &r->source) : NULL;
            const PgLabelEntry *tgt_lbl = lp_idmap_lookup(label_map, &r->target);
            const PgLabelEntry *rel_lbl = r->has_relation ? lp_idmap_lookup(label_map, &r->relation) : NULL;

            int64 wits = r->witnesses;
            if (r->has_claim) {
                const int64 *wc = lp_idmap_lookup(witness_counts, &r->claim);
                if (wc && *wc > 0) wits = *wc;
            }

            /* Compute bridged target occurrence indices. */
            RfOrigins tgt_occs = { 0 };
            for (int o = 0; o < nocc; o++) {
                int32 occ_num = o + 1;
                if (rf_origins_has(&r->origins, occ_num)) continue;
                if (lp_id_eq(&seeds[o], &r->target)) {
                    rf_origins_add(&tgt_occs, occ_num, rfctx);
                    continue;
                }
                if (r->vertices) {
                    for (int k = 0; k < r->nvertices; k++) {
                        if (lp_id_eq(&seeds[o], &r->vertices[k])) {
                            rf_origins_add(&tgt_occs, occ_num, rfctx);
                            break;
                        }
                    }
                }
            }
            const RfOrigins *eo = lp_idmap_lookup(entity_origins, &r->target);
            if (eo) {
                for (int k = 0; k < eo->n; k++) {
                    if (!rf_origins_has(&r->origins, eo->v[k]))
                        rf_origins_add(&tgt_occs, eo->v[k], rfctx);
                }
            }

            char fallback_hex[33];
            text *t_src = src_lbl ? cstring_to_text_with_len(src_lbl->text, src_lbl->len) : NULL;
            text *t_tgt = NULL;
            if (tgt_lbl) {
                t_tgt = cstring_to_text_with_len(tgt_lbl->text, tgt_lbl->len);
            } else {
                lp_id_hex(&r->target, fallback_hex);
                t_tgt = cstring_to_text_with_len(fallback_hex, 32);
            }
            text *t_rel = NULL;
            if (r->has_relation) {
                if (rel_lbl) t_rel = cstring_to_text_with_len(rel_lbl->text, rel_lbl->len);
                else { lp_id_hex(&r->relation, fallback_hex); t_rel = cstring_to_text_with_len(fallback_hex, 32); }
            }

            ArrayType *a_orig = int32_vec_to_array(r->origins.v, (size_t)r->origins.n);
            ArrayType *a_tgts = int32_vec_to_array(tgt_occs.v, (size_t)tgt_occs.n);
            ArrayType *a_bind = lp_id_vec_to_bytea_array(r->bindings, (size_t)r->nbindings);

            Datum v[21];
            bool nl[21] = { false };
            v[0]  = Int32GetDatum(r->round);
            v[1]  = Int32GetDatum(r->occ);
            v[2]  = r->has_source ? lp_id_bytea_datum(&r->source) : (Datum)0;  nl[2]  = !r->has_source;
            v[3]  = t_src ? PointerGetDatum(t_src) : (Datum)0;                 nl[3]  = (t_src == NULL);
            v[4]  = lp_id_bytea_datum(&r->target);
            v[5]  = PointerGetDatum(t_tgt);
            v[6]  = Int16GetDatum(r->route);
            v[7]  = r->has_claim ? lp_id_bytea_datum(&r->claim) : (Datum)0;    nl[7]  = !r->has_claim;
            v[8]  = r->has_relation ? lp_id_bytea_datum(&r->relation) : (Datum)0; nl[8] = !r->has_relation;
            v[9]  = t_rel ? PointerGetDatum(t_rel) : (Datum)0;                 nl[9]  = (t_rel == NULL);
            v[10] = r->route == 0 ? Float8GetDatum(r->rating) : (Datum)0;      nl[10] = (r->route != 0);
            v[11] = r->route == 0 ? Float8GetDatum(r->deviation) : (Datum)0;   nl[11] = (r->route != 0);
            v[12] = r->route == 0 ? Float8GetDatum(r->volatility) : (Datum)0;  nl[12] = (r->route != 0);
            v[13] = r->route == 0 ? Int64GetDatum(r->matches) : (Datum)0;      nl[13] = (r->route != 0);
            v[14] = r->route == 0 ? Int64GetDatum(wits) : (Datum)0;            nl[14] = (r->route != 0);
            v[15] = Int16GetDatum(r->tier);
            v[16] = r->has_distance ? Float8GetDatum(r->distance) : (Datum)0;  nl[16] = !r->has_distance;
            v[17] = r->has_outbound ? BoolGetDatum(r->outbound) : (Datum)0;    nl[17] = !r->has_outbound;
            v[18] = PointerGetDatum(a_orig);
            v[19] = PointerGetDatum(a_tgts);
            v[20] = PointerGetDatum(a_bind);

            set_put(fcinfo, ts, td, v, nl);

            if (!nl[2]) pfree(DatumGetPointer(v[2]));
            if (t_src) pfree(t_src);
            pfree(DatumGetPointer(v[4]));
            pfree(t_tgt);
            if (!nl[7]) pfree(DatumGetPointer(v[7]));
            if (!nl[8]) pfree(DatumGetPointer(v[8]));
            if (t_rel) pfree(t_rel);
            pfree(a_orig); pfree(a_tgts); pfree(a_bind);
        }
    }
    PG_FINALLY();
    {
        lp_idmap_free(label_map);
        lp_idmap_free(witness_counts);
        lp_idmap_free(entity_origins);
        lp_idmap_free(expanded_anchors);
        lp_idmap_free(seen_curves);
        lp_vec_free(&cv);
        lp_vec_free(&rows);
        MemoryContextDelete(rfctx);
        pfree(seeds);
    }
    PG_END_TRY();
    PG_RETURN_NULL();
}

/* ---------------------------------------------------------------- 2. laplace_leaderboard */

typedef struct {
    int32 band;
    const char *name;
    const char *const *preds;
    int npreds;
} GovernedBandDef;

static const char *const band1_preds[] = {
    "definition", "definition of the synset",
    "standard definition of the synset in the ILI",
    "English definition", "example", "example usage"
};
static const char *const band2_preds[] = {
    "hypernym", "instance_hypernym", "hyponym", "instance_hyponym",
    "subclass of", "instance of", "IS_A"
};
static const char *const band3_preds[] = {
    "lexical sense", "reference", "other form", "synonym",
    "similar", "antonym", "also", "derivation", "pertainym"
};
static const char *const band4_preds[] = {
    "meronym", "holonym", "mero_part", "holo_part",
    "mero_member", "holo_member", "mero_substance", "holo_substance",
    "PART_OF", "HAS_A"
};
static const char *const band5_preds[] = {
    "Part1", "Part2b", "Part2t", "variety of language",
    "pronunciation", "pronunciation of a form",
    "emoji name", "CLDR short name of sequence"
};

static const GovernedBandDef governed_bands[5] = {
    { 1, "definition",    band1_preds, (int)lengthof(band1_preds) },
    { 2, "taxonomy",      band2_preds, (int)lengthof(band2_preds) },
    { 3, "lexical",       band3_preds, (int)lengthof(band3_preds) },
    { 4, "part_whole",    band4_preds, (int)lengthof(band4_preds) },
    { 5, "language_code", band5_preds, (int)lengthof(band5_preds) }
};

typedef struct {
    lp_id id;
    uint32 run;
} LbPart;

typedef struct {
    LbPart *parts;
    int nparts;
} LbPartsEntry;

typedef struct {
    lp_idmap *paths_map; /* lp_id -> LbPartsEntry */
    MemoryContext ctx;
} LbPathCollect;

static void lb_path_visit(const lp_id *id, Datum path, Datum mask, void *context)
{
    (void)mask;
    LbPathCollect *c = context;
    bool fresh = false;
    LbPartsEntry *pe = lp_idmap_get(c->paths_map, id, &fresh);
    if (!fresh) return;
    Geo g = geo_of(path);
    size_t n = g.p.n;
    if (n > 64) n = 64;
    pe->nparts = (int)n;
    if (n == 0) { pe->parts = NULL; return; }
    pe->parts = MemoryContextAlloc(c->ctx, sizeof(LbPart) * n);
    for (size_t i = 0; i < n; i++) {
        pe->parts[i].id = lp_path_id(g.p, i);
        pe->parts[i].run = lp_path_run(g.p, i);
    }
}

static bool lb_path_middle_id(const LbPartsEntry *pe, lp_id *out_mid)
{
    if (!pe || pe->nparts == 0) return false;
    uint64 arity = 0;
    for (int i = 0; i < pe->nparts; i++) arity += pe->parts[i].run ? pe->parts[i].run : 1;
    if (arity < 3) return false;
    if (pe->parts[0].run >= 2) {
        *out_mid = pe->parts[0].id;
        return true;
    }
    if (pe->nparts >= 2) {
        *out_mid = pe->parts[1].id;
        return true;
    }
    return false;
}

typedef struct {
    lp_idmap *pred_set;
    int64 min_matches;
    lp_idmap *seen_claims;
    StandingRowVec *dst;
    lp_idmap *paths_map;
    MemoryContext ctx;
} LbBandGinCollect;

static void lb_band_gin_hit(const PgClaimHit *hit, void *context)
{
    LbBandGinCollect *c = context;
    if (hit->matches < c->min_matches || !hit->path) return;
    Geo g = geo_of(hit->path);
    if (g.p.n == 0) return;
    uint64 arity = 0;
    for (size_t i = 0; i < g.p.n && arity < 3; i++) arity += lp_path_run(g.p, i);
    if (arity < 3) return;
    lp_id mid = (lp_path_run(g.p, 0) >= 2) ? lp_path_id(g.p, 0)
              : (g.p.n >= 2 ? lp_path_id(g.p, 1) : lp_path_id(g.p, 0));
    if (lp_idmap_find(c->pred_set, &mid) < 0) return;

    bool fresh = false;
    lp_idmap_put(c->seen_claims, &hit->entity, &fresh);
    if (!fresh) return;

    StandingRow sr = {
        .claim = hit->entity,
        .rating = hit->rating,
        .deviation = hit->deviation,
        .volatility = hit->volatility,
        .confidence = standing_confidence(hit->rating, hit->deviation),
        .matches = hit->matches
    };
    lp_push(c->dst, sr);

    LbPathCollect pc = { c->paths_map, c->ctx };
    lb_path_visit(&hit->entity, hit->path, (Datum)0, &pc);
}

typedef struct {
    int32 band;
    const char *band_name;
    StandingRow st;
} LbFinalRow;

PG_FUNCTION_INFO_V1(laplace_leaderboard);
Datum laplace_leaderboard(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_leaderboard", &td);

    int32 lim = (PG_NARGS() > 1 && !PG_ARGISNULL(1)) ? PG_GETARG_INT32(1) : 50;
    int32 off = (PG_NARGS() > 2 && !PG_ARGISNULL(2)) ? PG_GETARG_INT32(2) : 0;
    int64 min_matches = (PG_NARGS() > 3 && !PG_ARGISNULL(3)) ? PG_GETARG_INT64(3) : 1;
    int32 max_text = (PG_NARGS() > 4 && !PG_ARGISNULL(4)) ? PG_GETARG_INT32(4) : 160;

    if (lim < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_leaderboard: lim must be >= 0 (got %d)", lim)));
    if (off < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_leaderboard: off must be >= 0 (got %d)", off)));
    if (min_matches < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_leaderboard: min_matches must be >= 0 (got " INT64_FORMAT ")", min_matches)));
    if (max_text < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_leaderboard: max_text must be >= 0 (got %d)", max_text)));

    int want_bands[5];
    int n_bands = 0;
    bool has_band_filter = false;
    if (!PG_ARGISNULL(0)) {
        ArrayType *ba = PG_GETARG_ARRAYTYPE_P(0);
        if (ARR_NDIM(ba) > 1)
            ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                            errmsg("laplace_leaderboard: bands must be 1-D int4[]")));
        Datum *elems = NULL; bool *nulls = NULL; int nb = 0;
        deconstruct_array(ba, INT4OID, sizeof(int32), true, TYPALIGN_INT, &elems, &nulls, &nb);
        bool seen_b[6] = { false };
        for (int i = 0; i < nb; i++) {
            if (nulls[i]) {
                pfree(elems); pfree(nulls);
                ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                                errmsg("laplace_leaderboard: bands array cannot contain NULL")));
            }
            int32 b = DatumGetInt32(elems[i]);
            if (b < 1 || b > 5) {
                pfree(elems); pfree(nulls);
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("laplace_leaderboard: unknown relation band %d (valid bands are 1..5)", b)));
            }
            if (!seen_b[b]) {
                seen_b[b] = true;
                want_bands[n_bands++] = b;
            }
        }
        if (elems) pfree(elems);
        if (nulls) pfree(nulls);
        if (n_bands > 0) {
            has_band_filter = true;
            /* Sort requested bands ascending for deterministic output. */
            for (int a = 1; a < n_bands; a++) {
                int k = want_bands[a], j = a - 1;
                while (j >= 0 && want_bands[j] > k) { want_bands[j + 1] = want_bands[j]; j--; }
                want_bands[j + 1] = k;
            }
        }
    }

    if (lim == 0) PG_RETURN_NULL();

    MemoryContext lbctx = AllocSetContextCreate(CurrentMemoryContext, "laplace_leaderboard", ALLOCSET_DEFAULT_SIZES);
    StandingMerge merge = { 0 };
    lp_vec(LbFinalRow) final_rows = { 0 };
    lp_idmap *paths_map = lp_idmap_sized(sizeof(LbPartsEntry));
    lp_idmap *meta_map = lp_idmap_sized(sizeof(PgEntityMeta));
    lp_idmap *label_map = lp_idmap_sized(sizeof(PgLabelEntry));

    PG_TRY();
    {
        standing_merge_open(&merge, min_matches);
        size_t need = (size_t)off + (size_t)lim;

        if (!has_band_filter) {
            StandingRow sr;
            size_t skipped = 0;
            while (final_rows.n < (size_t)lim && standing_merge_next(&merge, &sr)) {
                CHECK_FOR_INTERRUPTS();
                if (skipped < (size_t)off) { skipped++; continue; }
                LbFinalRow fr = { 0, "consensus", sr };
                lp_push(&final_rows, fr);
            }
            standing_merge_close(&merge);
        } else {
            /* Resolve governed predicate IDs for each requested band. */
            lp_text *tx = lp_pg_text_context();
            lp_idmap *pred_to_mask = lp_idmap_sized(sizeof(uint32));
            lp_idmap *band_pred_sets[5] = { NULL };
            lp_vec(lp_id) band_pred_ids[5] = { { 0 } };
            StandingRowVec band_rows[5] = { { 0 } };
            lp_idmap *band_seen[5] = { NULL };

            for (int bi = 0; bi < n_bands; bi++) {
                const GovernedBandDef *def = &governed_bands[want_bands[bi] - 1];
                band_pred_sets[bi] = lp_idmap_new();
                band_seen[bi] = lp_idmap_new();
                for (int k = 0; k < def->npreds; k++) {
                    lp_id pid;
                    if (!lp_text_id(tx, def->preds[k], strlen(def->preds[k]), &pid)) continue;
                    lp_idmap_put(band_pred_sets[bi], &pid, NULL);
                    lp_push(&band_pred_ids[bi], pid);
                    bool fresh = false;
                    uint32 *bm = lp_idmap_get(pred_to_mask, &pid, &fresh);
                    if (fresh) *bm = 0;
                    *bm |= (1u << bi);
                }
            }

            /* Phase 1: stream consensus_standing in batches until every requested band is full
             * or the bounded streaming scan budget is reached. */
            size_t scan_budget = Max((size_t)8192, need * 64);
            size_t scanned = 0;
            const size_t BATCH = 256;
            StandingRow *batch = palloc(sizeof(StandingRow) * BATCH);
            lp_id *batch_ids = palloc(sizeof(lp_id) * BATCH);

            while (scanned < scan_budget) {
                CHECK_FOR_INTERRUPTS();
                bool all_full = true;
                for (int bi = 0; bi < n_bands; bi++) {
                    if (band_rows[bi].n < need) { all_full = false; break; }
                }
                if (all_full) break;

                size_t nb = 0;
                while (nb < BATCH && scanned < scan_budget && standing_merge_next(&merge, &batch[nb])) {
                    batch_ids[nb] = batch[nb].claim;
                    nb++;
                    scanned++;
                }
                if (nb == 0) break;

                LbPathCollect pc = { paths_map, lbctx };
                lp_pg_lookup_paths(batch_ids, nb, false, lb_path_visit, &pc);

                for (size_t i = 0; i < nb; i++) {
                    const LbPartsEntry *pe = lp_idmap_lookup(paths_map, &batch[i].claim);
                    lp_id mid;
                    if (!lb_path_middle_id(pe, &mid)) continue;
                    const uint32 *bm = lp_idmap_lookup(pred_to_mask, &mid);
                    if (!bm) continue;
                    for (int bi = 0; bi < n_bands; bi++) {
                        if (!((*bm) & (1u << bi))) continue;
                        if (band_rows[bi].n >= need) continue;
                        bool fresh = false;
                        lp_idmap_put(band_seen[bi], &batch[i].claim, &fresh);
                        if (fresh) lp_push(&band_rows[bi], batch[i]);
                    }
                }
            }
            pfree(batch_ids);
            pfree(batch);
            standing_merge_close(&merge);

            /* Phase 2: targeted GIN fallback for any sparse band still short of off + lim. */
            for (int bi = 0; bi < n_bands; bi++) {
                if (band_rows[bi].n >= need || band_pred_ids[bi].n == 0) continue;
                int64 gin_fan = Max((int64)need * 8, (int64)256);
                LbBandGinCollect gc = {
                    .pred_set = band_pred_sets[bi],
                    .min_matches = min_matches,
                    .seen_claims = band_seen[bi],
                    .dst = &band_rows[bi],
                    .paths_map = paths_map,
                    .ctx = lbctx
                };
                lp_pg_claims_each_direct(band_pred_ids[bi].v, (int)band_pred_ids[bi].n,
                                         gin_fan, UINT64_C(1),
                                         NULL, 0, 0, NULL, 0, 0, NULL, 0,
                                         lb_band_gin_hit, &gc);
                if (band_rows[bi].n > 1)
                    lp_sort(band_rows[bi].v, band_rows[bi].n, sizeof(StandingRow), cmp_standing_row_desc);
            }

            /* Collect [off, off + lim) per requested band. */
            for (int bi = 0; bi < n_bands; bi++) {
                const GovernedBandDef *def = &governed_bands[want_bands[bi] - 1];
                for (size_t i = (size_t)off; i < band_rows[bi].n && i < need; i++) {
                    LbFinalRow fr = { def->band, def->name, band_rows[bi].v[i] };
                    lp_push(&final_rows, fr);
                }
                lp_vec_free(&band_rows[bi]);
                lp_vec_free(&band_pred_ids[bi]);
                lp_idmap_free(band_pred_sets[bi]);
                lp_idmap_free(band_seen[bi]);
            }
            lp_idmap_free(pred_to_mask);
        }

        if (final_rows.n > 0) {
            /* Batch-fetch entity(tier, mask), missing physicality paths, and all labels. */
            lp_id *ids = palloc(sizeof(lp_id) * final_rows.n);
            lp_vec(lp_id) missing_paths = { 0 };
            for (size_t i = 0; i < final_rows.n; i++) {
                ids[i] = final_rows.v[i].st.claim;
                if (lp_idmap_find(paths_map, &ids[i]) < 0) lp_push(&missing_paths, ids[i]);
            }
            lp_pg_lookup_entity_meta(ids, final_rows.n, meta_map);
            if (missing_paths.n > 0) {
                LbPathCollect pc = { paths_map, lbctx };
                lp_pg_lookup_paths(missing_paths.v, missing_paths.n, false, lb_path_visit, &pc);
            }
            lp_vec_free(&missing_paths);
            pfree(ids);

            lp_idmap *all_lbl_ids = lp_idmap_new();
            for (size_t i = 0; i < final_rows.n; i++) {
                lp_idmap_put(all_lbl_ids, &final_rows.v[i].st.claim, NULL);
                const LbPartsEntry *pe = lp_idmap_lookup(paths_map, &final_rows.v[i].st.claim);
                if (pe) {
                    for (int k = 0; k < pe->nparts; k++)
                        lp_idmap_put(all_lbl_ids, &pe->parts[k].id, NULL);
                }
            }
            if (lp_idmap_count(all_lbl_ids) > 0) {
                lp_pg_realize_labels_map(lp_idmap_keys(all_lbl_ids), lp_idmap_count(all_lbl_ids),
                                         max_text, false, lbctx, label_map);
            }
            lp_idmap_free(all_lbl_ids);

            /* Emit final rows. */
            for (size_t i = 0; i < final_rows.n; i++) {
                CHECK_FOR_INTERRUPTS();
                const LbFinalRow *fr = &final_rows.v[i];
                const PgEntityMeta *meta = lp_idmap_lookup(meta_map, &fr->st.claim);
                int16 tier = (meta && meta->present) ? meta->tier : 0;
                int64 mask = (meta && meta->present) ? meta->mask : 0;

                const PgLabelEntry *lbl = lp_idmap_lookup(label_map, &fr->st.claim);
                char hex[33];
                text *t_lbl = NULL;
                if (lbl) {
                    t_lbl = cstring_to_text_with_len(lbl->text, lbl->len);
                } else {
                    lp_id_hex(&fr->st.claim, hex);
                    t_lbl = cstring_to_text_with_len(hex, 32);
                }

                StringInfoData pj;
                initStringInfo(&pj);
                appendStringInfoChar(&pj, '[');
                const LbPartsEntry *pe = lp_idmap_lookup(paths_map, &fr->st.claim);
                if (pe) {
                    for (int k = 0; k < pe->nparts; k++) {
                        if (k > 0) appendStringInfoChar(&pj, ',');
                        lp_id_hex(&pe->parts[k].id, hex);
                        const PgLabelEntry *pl = lp_idmap_lookup(label_map, &pe->parts[k].id);
                        appendStringInfo(&pj, "{\"id\":\"%s\",\"label\":", hex);
                        if (pl && !pl->is_hex) json_append_escaped(&pj, pl->text, pl->len);
                        else json_append_escaped(&pj, hex, 32);
                        appendStringInfo(&pj, ",\"run\":%u}", pe->parts[k].run ? pe->parts[k].run : 1u);
                    }
                }
                appendStringInfoChar(&pj, ']');

                text *t_band_name = cstring_to_text(fr->band_name);
                text *t_parts = cstring_to_text_with_len(pj.data, pj.len);

                Datum v[11] = {
                    lp_id_bytea_datum(&fr->st.claim),
                    Int16GetDatum(tier),
                    Int64GetDatum(mask),
                    Float8GetDatum(fr->st.rating),
                    Float8GetDatum(fr->st.deviation),
                    Int64GetDatum(fr->st.matches),
                    PointerGetDatum(t_lbl),
                    Int32GetDatum(fr->band),
                    PointerGetDatum(t_band_name),
                    Float8GetDatum(fr->st.confidence),
                    PointerGetDatum(t_parts)
                };
                bool nl[11] = { false };
                set_put(fcinfo, ts, td, v, nl);

                pfree(DatumGetPointer(v[0]));
                pfree(t_lbl);
                pfree(t_band_name);
                pfree(t_parts);
                pfree(pj.data);
            }
        }
    }
    PG_FINALLY();
    {
        standing_merge_close(&merge);
        lp_idmap_free(label_map);
        lp_idmap_free(meta_map);
        lp_idmap_free(paths_map);
        lp_vec_free(&final_rows);
        MemoryContextDelete(lbctx);
    }
    PG_END_TRY();
    PG_RETURN_NULL();
}

/* ---------------------------------------------------------------- 3. laplace_browse_entities */

enum {
    BROWSE_KIND_EXACT = 0,
    BROWSE_KIND_CASE_VARIANT = 1,
    BROWSE_KIND_PREFIX = 2,
    BROWSE_KIND_CONSTITUENT = 3,
    BROWSE_KIND_MEMBER = 4,
    BROWSE_KIND_MASK = 5,
    BROWSE_KIND_STANDING = 6,
    BROWSE_KIND_ENTITY = 7
};

static const char *browse_kind_name(int kind)
{
    switch (kind) {
        case BROWSE_KIND_EXACT:        return "exact";
        case BROWSE_KIND_CASE_VARIANT: return "case_variant";
        case BROWSE_KIND_PREFIX:       return "prefix";
        case BROWSE_KIND_CONSTITUENT:  return "constituent";
        case BROWSE_KIND_MEMBER:       return "member";
        case BROWSE_KIND_MASK:         return "mask";
        case BROWSE_KIND_STANDING:     return "standing";
        default:                       return "entity";
    }
}

typedef struct {
    lp_id id;
    int16 tier;
    int64 mask;
    bool has_standing;
    double rating;
    double deviation;
    double confidence;
    int64 matches;
    double sal_score;
    int match_kind;
} BrowseCand;

static int cmp_browse_cand(const void *a, const void *b)
{
    const BrowseCand *x = a, *y = b;
    if (x->match_kind != y->match_kind) return x->match_kind < y->match_kind ? -1 : 1;
    if (x->sal_score != y->sal_score) return x->sal_score > y->sal_score ? -1 : 1;
    if (x->has_standing != y->has_standing) return x->has_standing ? -1 : 1;
    if (x->has_standing) {
        if (x->confidence != y->confidence) return x->confidence > y->confidence ? -1 : 1;
        if (x->matches != y->matches) return x->matches > y->matches ? -1 : 1;
    }
    if (x->tier != y->tier) return x->tier < y->tier ? -1 : 1;
    return memcmp(x->id.b, y->id.b, sizeof(lp_id));
}

typedef struct {
    bool has_tiers;
    bool want_tier[256];
    int16 max_tier_filter;
    bool has_mask;
    lp_mask want_mask;
    ArrayType *mask_arr;
} BrowseFilter;

static inline bool browse_tier_ok(const BrowseFilter *f, int16 tier)
{
    if (!f->has_tiers) return true;
    return tier >= 0 && tier <= 255 && f->want_tier[tier];
}

static inline bool browse_part_tier_ok(const BrowseFilter *f, int16 lo, int16 hi)
{
    if (!f->has_tiers) return true;
    int a = lo < 0 ? 0 : (lo > 255 ? 255 : lo);
    int b = hi < 0 ? 0 : (hi > 255 ? 255 : hi);
    for (int t = a; t <= b; t++)
        if (f->want_tier[t]) return true;
    return false;
}

static inline bool browse_mask_ok(const BrowseFilter *f, int64 mask)
{
    if (!f->has_mask) return true;
    return (((lp_mask)mask) & f->want_mask) != 0;
}

typedef struct {
    lp_idmap *cands; /* lp_id -> BrowseCand */
    double sal_score;
    const BrowseFilter *filter;
} BrowseHolderCollect;

static void browse_holder_hit(const PgHolderHit *hit, void *context)
{
    BrowseHolderCollect *c = context;
    if (hit->hub || !hit->path) return;
    if (c->filter->has_tiers && !browse_tier_ok(c->filter, hit->tier)) return;
    bool fresh = false;
    BrowseCand *bc = lp_idmap_get(c->cands, &hit->entity, &fresh);
    if (fresh) {
        memset(bc, 0, sizeof(*bc));
        bc->id = hit->entity;
        bc->tier = hit->tier;
        bc->sal_score = c->sal_score;
        bc->match_kind = BROWSE_KIND_MEMBER;
    }
}

static bool ascii_is_ws_only(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v')
            return false;
    }
    return true;
}

static bool str_starts_with_ci(const char *hay, int hay_len, const char *needle, int needle_len)
{
    if (needle_len <= 0 || hay_len < needle_len) return false;
    for (int i = 0; i < needle_len; i++) {
        unsigned char a = (unsigned char)hay[i];
        unsigned char b = (unsigned char)needle[i];
        if (tolower(a) != tolower(b)) return false;
    }
    return true;
}

static Oid find_entity_mask_gin(Relation part, AttrNumber a_mask, int *out_col, Oid *out_op)
{
    Oid mask_type = get_atttype(RelationGetRelid(part), a_mask);
    List *idxs = RelationGetIndexList(part);
    ListCell *lc;
    Oid found = InvalidOid;
    foreach(lc, idxs) {
        Oid ioid = lfirst_oid(lc);
        Relation idx = index_open(ioid, AccessShareLock);
        if (idx->rd_rel->relam == GIN_AM_OID && idx->rd_index &&
            idx->rd_index->indisvalid && idx->rd_index->indisready && idx->rd_index->indislive &&
            heap_attisnull(idx->rd_indextuple, Anum_pg_index_indpred, NULL)) {
            for (int i = 0; i < idx->rd_index->indnkeyatts; i++) {
                if (idx->rd_index->indkey.values[i] == a_mask) {
                    /* Strategy 3 = ?| (mask has ANY bit in int2[]) */
                    Oid op = get_opfamily_member(idx->rd_opfamily[i], mask_type, INT2ARRAYOID, 3);
                    if (OidIsValid(op)) {
                        found = ioid;
                        *out_col = i + 1;
                        *out_op = get_opcode(op);
                        break;
                    }
                }
            }
        }
        index_close(idx, NoLock);
        if (OidIsValid(found)) break;
    }
    list_free(idxs);
    return found;
}

static Oid find_entity_browse_btree(Relation part, AttrNumber a_tier, AttrNumber a_id)
{
    List *idxs = RelationGetIndexList(part);
    ListCell *lc;
    Oid tier_idx = InvalidOid, id_idx = InvalidOid;
    foreach(lc, idxs) {
        Oid ioid = lfirst_oid(lc);
        Relation idx = index_open(ioid, AccessShareLock);
        if (idx->rd_rel->relam == BTREE_AM_OID && idx->rd_index &&
            idx->rd_index->indisvalid && idx->rd_index->indisready && idx->rd_index->indislive &&
            idx->rd_index->indnkeyatts >= 1) {
            if (!OidIsValid(tier_idx) && idx->rd_index->indkey.values[0] == a_tier)
                tier_idx = ioid;
            if (!OidIsValid(id_idx) && idx->rd_index->indkey.values[0] == a_id)
                id_idx = ioid;
        }
        index_close(idx, NoLock);
    }
    list_free(idxs);
    return OidIsValid(tier_idx) ? tier_idx : id_idx;
}

PG_FUNCTION_INFO_V1(laplace_browse_entities);
Datum laplace_browse_entities(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_browse_entities", &td);

    int32 lim = (PG_NARGS() > 3 && !PG_ARGISNULL(3)) ? PG_GETARG_INT32(3) : 50;
    int32 off = (PG_NARGS() > 4 && !PG_ARGISNULL(4)) ? PG_GETARG_INT32(4) : 0;
    int32 max_text = (PG_NARGS() > 5 && !PG_ARGISNULL(5)) ? PG_GETARG_INT32(5) : 160;

    if (lim < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_browse_entities: lim must be >= 0 (got %d)", lim)));
    if (off < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_browse_entities: off must be >= 0 (got %d)", off)));
    if (max_text < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_browse_entities: max_text must be >= 0 (got %d)", max_text)));

    BrowseFilter filter = { 0 };
    if (!PG_ARGISNULL(0)) {
        ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);
        if (ARR_NDIM(ta) > 1)
            ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                            errmsg("laplace_browse_entities: tiers must be 1-D smallint[]")));
        Datum *elems = NULL; bool *nulls = NULL; int nt = 0;
        deconstruct_array(ta, INT2OID, sizeof(int16), true, TYPALIGN_SHORT, &elems, &nulls, &nt);
        filter.has_tiers = true;
        for (int i = 0; i < nt; i++) {
            if (nulls[i]) {
                pfree(elems); pfree(nulls);
                ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                                errmsg("laplace_browse_entities: tiers cannot contain NULL")));
            }
            int16 t = DatumGetInt16(elems[i]);
            if (t < 0 || t > 255) {
                pfree(elems); pfree(nulls);
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("laplace_browse_entities: tier %d out of range [0, 255]", (int)t)));
            }
            filter.want_tier[t] = true;
            if (t > filter.max_tier_filter) filter.max_tier_filter = t;
        }
        if (elems) pfree(elems);
        if (nulls) pfree(nulls);
        if (nt == 0) PG_RETURN_NULL();
    }

    if (!PG_ARGISNULL(1)) {
        ArrayType *ma = PG_GETARG_ARRAYTYPE_P(1);
        if (ARR_NDIM(ma) > 1)
            ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                            errmsg("laplace_browse_entities: mask_bits must be 1-D int4[]")));
        Datum *elems = NULL; bool *nulls = NULL; int nm = 0;
        deconstruct_array(ma, INT4OID, sizeof(int32), true, TYPALIGN_INT, &elems, &nulls, &nm);
        filter.has_mask = true;
        Datum bit_vals[64];
        int nbits = 0;
        for (int i = 0; i < nm; i++) {
            if (nulls[i]) {
                pfree(elems); pfree(nulls);
                ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                                errmsg("laplace_browse_entities: mask_bits cannot contain NULL")));
            }
            int32 b = DatumGetInt32(elems[i]);
            if (b < 0 || b >= 64) {
                pfree(elems); pfree(nulls);
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("laplace_browse_entities: mask bit %d out of range [0, 63]", b)));
            }
            if ((filter.want_mask & (UINT64_C(1) << b)) == 0) {
                filter.want_mask |= (UINT64_C(1) << b);
                bit_vals[nbits++] = Int16GetDatum((int16)b);
            }
        }
        if (elems) pfree(elems);
        if (nulls) pfree(nulls);
        if (nbits == 0) PG_RETURN_NULL();
        filter.mask_arr = construct_array(bit_vals, nbits, INT2OID, sizeof(int16), true, TYPALIGN_SHORT);
    }

    char *prefix = NULL;
    int prefix_len = 0;
    if (PG_NARGS() > 2 && !PG_ARGISNULL(2)) {
        text *pt = PG_GETARG_TEXT_PP(2);
        int raw_len = VARSIZE_ANY_EXHDR(pt);
        if (raw_len > 0) {
            prefix = pnstrdup(VARDATA_ANY(pt), (Size)raw_len);
            prefix_len = raw_len;
        }
    }

    if (lim == 0) {
        if (prefix) pfree(prefix);
        if (filter.mask_arr) pfree(filter.mask_arr);
        PG_RETURN_NULL();
    }

    /* Check SELECT privilege on entity once up front. */
    {
        const char *selected[] = { "id", "tier", "mask" };
        const char *column = NULL;
        Relation ep = lp_pg_read_parent(lp_pg_relation("entity"), &column, selected, 3);
        table_close(ep, NoLock);
    }

    MemoryContext bctx = AllocSetContextCreate(CurrentMemoryContext, "laplace_browse_entities", ALLOCSET_DEFAULT_SIZES);
    lp_idmap *cand_map = lp_idmap_sized(sizeof(BrowseCand));
    lp_idmap *meta_map = lp_idmap_sized(sizeof(PgEntityMeta));
    lp_idmap *st_map = lp_idmap_sized(sizeof(PgStandingEntry));
    lp_idmap *label_map = lp_idmap_sized(sizeof(PgLabelEntry));
    StandingMerge merge = { 0 };
    int64 total_count = 0;

    PG_TRY();
    {
        size_t need = (size_t)off + (size_t)lim;

        if (prefix != NULL) {
            /* ---- Path 1: prefix / named-entity indexed lookup & holder discovery ---- */
            lp_id hex_id;
            if (prefix_len == 32 && lp_id_unhex(prefix, &hex_id)) {
                BrowseCand *bc = lp_idmap_get(cand_map, &hex_id, NULL);
                memset(bc, 0, sizeof(*bc));
                bc->id = hex_id;
                bc->sal_score = 1e6;
                bc->match_kind = BROWSE_KIND_EXACT;
            } else {
                lp_text *tx = lp_pg_text_context();
                lp_id exact_id = { { 0 } };
                bool has_exact = lp_text_id(tx, prefix, (size_t)prefix_len, &exact_id);
                lp_vec(lp_id) lookup_ids = { 0 };

                if (has_exact) {
                    BrowseCand *bc = lp_idmap_get(cand_map, &exact_id, NULL);
                    memset(bc, 0, sizeof(*bc));
                    bc->id = exact_id;
                    bc->sal_score = 1e6;
                    bc->match_kind = BROWSE_KIND_EXACT;
                    lp_push(&lookup_ids, exact_id);
                }

                /* Case variants (lowercase, Titlecase, UPPERCASE) preserving pure BLAKE3 identity. */
                char *vbuf = palloc((Size)prefix_len + 1);
                for (int mode = 0; mode < 3; mode++) {
                    for (int i = 0; i < prefix_len; i++) {
                        unsigned char c = (unsigned char)prefix[i];
                        if (mode == 0) vbuf[i] = (char)tolower(c);
                        else if (mode == 1) vbuf[i] = (char)(i == 0 ? toupper(c) : tolower(c));
                        else vbuf[i] = (char)toupper(c);
                    }
                    vbuf[prefix_len] = '\0';
                    lp_id vid;
                    if (lp_text_id(tx, vbuf, (size_t)prefix_len, &vid) &&
                        (!has_exact || !lp_id_eq(&vid, &exact_id))) {
                        bool fresh = false;
                        BrowseCand *bc = lp_idmap_get(cand_map, &vid, &fresh);
                        if (fresh) {
                            memset(bc, 0, sizeof(*bc));
                            bc->id = vid;
                            bc->sal_score = 5e5;
                            bc->match_kind = BROWSE_KIND_CASE_VARIANT;
                            lp_push(&lookup_ids, vid);
                        }
                    }
                }
                pfree(vbuf);

                /* UAX #29 segments for multi-word queries. */
                lp_ref seg_stack[64];
                size_t nseg = 0;
                (void)lp_text_parts(tx, (const uint8_t *)prefix, (size_t)prefix_len, seg_stack, 64, &nseg);
                if (nseg > 1) {
                    size_t take = Min(nseg, (size_t)64);
                    for (size_t i = 0; i < take; i++) {
                        if (lp_idmap_find(cand_map, &seg_stack[i].id) >= 0) continue;
                        /* Skip single-codepoint ASCII whitespace segments. */
                        int32 cp = lp_tier0_codepoint(lp_pg_tier0(), &seg_stack[i].id);
                        if (cp >= 0 && (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r')) continue;
                        bool fresh = false;
                        BrowseCand *bc = lp_idmap_get(cand_map, &seg_stack[i].id, &fresh);
                        if (fresh) {
                            memset(bc, 0, sizeof(*bc));
                            bc->id = seg_stack[i].id;
                            bc->match_kind = BROWSE_KIND_CONSTITUENT;
                            lp_push(&lookup_ids, seg_stack[i].id);
                        }
                    }
                }

                /* Compute salience over lookup_ids and select salient members for GIN holder discovery. */
                if (lookup_ids.n > 0) {
                    size_t nl = lp_ids_unique(lookup_ids.v, lookup_ids.n);
                    PgSalience *sal = palloc0(sizeof(PgSalience) * nl);
                    lp_pg_salience(lookup_ids.v, (int)nl, NULL, 0, sal);
                    double max_sal = 0.0;
                    for (size_t i = 0; i < nl; i++) {
                        if (sal[i].salience > max_sal) max_sal = sal[i].salience;
                        BrowseCand *bc = lp_idmap_lookup(cand_map, &lookup_ids.v[i]);
                        if (bc && bc->match_kind == BROWSE_KIND_CONSTITUENT)
                            bc->sal_score = sal[i].salience;
                    }
                    lp_vec(lp_id) salient_ids = { 0 };
                    for (size_t i = 0; i < nl; i++) {
                        if (nl <= 1 || max_sal <= 0.0 || sal[i].salience >= 0.40 * max_sal)
                            lp_push(&salient_ids, lookup_ids.v[i]);
                    }
                    pfree(sal);

                    if (salient_ids.n > 0) {
                        int64 holder_cap = Max((int64)256, (int64)need * 8);
                        BrowseHolderCollect hc = { cand_map, max_sal, &filter };
                        lp_pg_holders_direct(salient_ids.v, (int)salient_ids.n, NULL, 0,
                                             true, -1, holder_cap, 3, 0, 0, browse_holder_hit, &hc);
                    }
                    lp_vec_free(&salient_ids);
                }
                lp_vec_free(&lookup_ids);
            }

            /* Batch-verify entity existence, tier, mask, and consensus standing. */
            size_t nc = lp_idmap_count(cand_map);
            if (nc > 0) {
                lp_pg_lookup_entity_meta(lp_idmap_keys(cand_map), nc, meta_map);
                lp_pg_lookup_standings(lp_idmap_keys(cand_map), nc, st_map);
            }

            lp_vec(BrowseCand) qualified = { 0 };
            for (size_t i = 0; i < nc; i++) {
                BrowseCand bc = *(BrowseCand *)lp_idmap_at(cand_map, i);
                const PgEntityMeta *em = lp_idmap_lookup(meta_map, &bc.id);
                if (!em || !em->present) continue;
                bc.tier = em->tier;
                bc.mask = em->mask;
                if (!browse_tier_ok(&filter, bc.tier)) continue;
                if (!browse_mask_ok(&filter, bc.mask)) continue;
                const PgStandingEntry *se = lp_idmap_lookup(st_map, &bc.id);
                if (se && se->present) {
                    bc.has_standing = true;
                    bc.rating = se->rating;
                    bc.deviation = se->deviation;
                    bc.confidence = standing_confidence(se->rating, se->deviation);
                    bc.matches = se->matches;
                }
                lp_push(&qualified, bc);
            }

            /* Realize labels for prefix-boost check (on member hits) and output. */
            if (qualified.n > 0) {
                lp_id *qids = palloc(sizeof(lp_id) * qualified.n);
                for (size_t i = 0; i < qualified.n; i++) qids[i] = qualified.v[i].id;
                lp_pg_realize_labels_map(qids, qualified.n, max_text, false, bctx, label_map);
                pfree(qids);

                if (!ascii_is_ws_only(prefix, (size_t)prefix_len)) {
                    for (size_t i = 0; i < qualified.n; i++) {
                        if (qualified.v[i].match_kind == BROWSE_KIND_MEMBER) {
                            const PgLabelEntry *le = lp_idmap_lookup(label_map, &qualified.v[i].id);
                            if (le && !le->is_hex && str_starts_with_ci(le->text, le->len, prefix, prefix_len))
                                qualified.v[i].match_kind = BROWSE_KIND_PREFIX;
                        }
                    }
                }
                if (qualified.n > 1)
                    lp_sort(qualified.v, qualified.n, sizeof(BrowseCand), cmp_browse_cand);
            }

            total_count = (int64)qualified.n;
            for (size_t i = (size_t)off; i < qualified.n && i < need; i++) {
                CHECK_FOR_INTERRUPTS();
                const BrowseCand *bc = &qualified.v[i];
                const PgLabelEntry *le = lp_idmap_lookup(label_map, &bc->id);
                char hex[33];
                text *t_lbl = NULL;
                if (le) t_lbl = cstring_to_text_with_len(le->text, le->len);
                else { lp_id_hex(&bc->id, hex); t_lbl = cstring_to_text_with_len(hex, 32); }
                text *t_kind = cstring_to_text(browse_kind_name(bc->match_kind));

                Datum v[10] = {
                    lp_id_bytea_datum(&bc->id),
                    Int16GetDatum(bc->tier),
                    Int64GetDatum(bc->mask),
                    bc->has_standing ? Float8GetDatum(bc->rating) : (Datum)0,
                    bc->has_standing ? Float8GetDatum(bc->deviation) : (Datum)0,
                    bc->has_standing ? Float8GetDatum(bc->confidence) : (Datum)0,
                    Int64GetDatum(bc->has_standing ? bc->matches : 0),
                    PointerGetDatum(t_lbl),
                    PointerGetDatum(t_kind),
                    Int64GetDatum(total_count)
                };
                bool nl[10] = { false, false, false, !bc->has_standing, !bc->has_standing, !bc->has_standing, false, false, false, false };
                set_put(fcinfo, ts, td, v, nl);
                pfree(DatumGetPointer(v[0]));
                pfree(t_lbl);
                pfree(t_kind);
            }
            lp_vec_free(&qualified);
        } else if (filter.has_mask) {
            /* ---- Path 2: entity_mask GIN (?|) bitmap scan across non-pruned entity partitions ---- */
            if (GinFuzzySearchLimit)
                ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                                errmsg("laplace_browse_entities: requires exact GIN search (gin_fuzzy_search_limit=0)")));

            const Oid *parts = NULL;
            const int16 *min_t = NULL, *max_t = NULL;
            int nparts = lp_pg_canonical_partitions("entity", &parts, NULL, NULL, NULL);
            (void)lp_pg_canonical_tier_bounds("entity", &min_t, &max_t);
            Snapshot snap = GetActiveSnapshot();
            Size bm_mem = Min((Size)work_mem * 1024, (Size)16 * 1024 * 1024);
            size_t pool_cap = Max((size_t)4096, need * 16);
            lp_vec(BrowseCand) pool = { 0 };

            for (int p = 0; p < nparts; p++) {
                CHECK_FOR_INTERRUPTS();
                if (min_t && max_t && !browse_part_tier_ok(&filter, min_t[p], max_t[p]))
                    continue;
                Relation part = table_open(parts[p], AccessShareLock);
                if (RelationGetNumberOfBlocks(part) == 0) {
                    table_close(part, NoLock);
                    continue;
                }
                AttrNumber a_id = get_attnum(parts[p], "id");
                AttrNumber a_tier = get_attnum(parts[p], "tier");
                AttrNumber a_mask = get_attnum(parts[p], "mask");

                int mask_col = 0;
                Oid mask_op = InvalidOid;
                Oid gin_oid = find_entity_mask_gin(part, a_mask, &mask_col, &mask_op);

                if (OidIsValid(gin_oid)) {
                    Relation idx = index_open(gin_oid, AccessShareLock);
                    TIDBitmap *bm = tbm_create(bm_mem, NULL);
                    ScanKeyData sk;
                    ScanKeyEntryInitialize(&sk, 0, mask_col, 3, INT2ARRAYOID,
                                           idx->rd_indcollation[mask_col - 1], mask_op,
                                           PointerGetDatum(filter.mask_arr));
                    IndexScanDesc iscan = index_beginscan_bitmap(idx, snap, NULL, 1);
                    index_rescan(iscan, &sk, 1, NULL, 0);
                    index_getbitmap(iscan, bm);
                    if (!tbm_is_empty(bm)) {
                        TableScanDesc heap = table_beginscan_bm(part, snap, 0, NULL);
                        heap->st.rs_tbmiterator = tbm_begin_iterate(bm, NULL, InvalidDsaPointer);
                        TupleTableSlot *slot = table_slot_create(part, NULL);
                        bool recheck = false;
                        uint64 lossy = 0, exact = 0;
                        while (table_scan_bitmap_next_tuple(heap, slot, &recheck, &lossy, &exact)) {
                            CHECK_FOR_INTERRUPTS();
                            bool ni = true, nt = true, nm = true;
                            Datum dm = slot_getattr(slot, a_mask, &nm);
                            Datum dt = slot_getattr(slot, a_tier, &nt);
                            if (!nm && !nt && browse_mask_ok(&filter, DatumGetInt64(dm)) &&
                                browse_tier_ok(&filter, DatumGetInt16(dt))) {
                                total_count++;
                                if (pool.n < pool_cap) {
                                    Datum di = slot_getattr(slot, a_id, &ni);
                                    if (!ni) {
                                        BrowseCand bc = { 0 };
                                        memcpy(bc.id.b, DatumGetPointer(di), sizeof(lp_id));
                                        bc.tier = DatumGetInt16(dt);
                                        bc.mask = DatumGetInt64(dm);
                                        bc.match_kind = BROWSE_KIND_MASK;
                                        lp_push(&pool, bc);
                                    }
                                }
                            }
                            ExecClearTuple(slot);
                        }
                        ExecDropSingleTupleTableSlot(slot);
                        tbm_end_iterate(&heap->st.rs_tbmiterator);
                        table_endscan(heap);
                    }
                    tbm_free(bm);
                    index_endscan(iscan);
                    index_close(idx, NoLock);
                } else {
                    TableScanDesc tscan = table_beginscan(part, snap, 0, NULL);
                    TupleTableSlot *slot = table_slot_create(part, NULL);
                    while (table_scan_getnextslot(tscan, ForwardScanDirection, slot)) {
                        CHECK_FOR_INTERRUPTS();
                        bool ni = true, nt = true, nm = true;
                        Datum dm = slot_getattr(slot, a_mask, &nm);
                        Datum dt = slot_getattr(slot, a_tier, &nt);
                        if (!nm && !nt && browse_mask_ok(&filter, DatumGetInt64(dm)) &&
                            browse_tier_ok(&filter, DatumGetInt16(dt))) {
                            total_count++;
                            if (pool.n < pool_cap) {
                                Datum di = slot_getattr(slot, a_id, &ni);
                                if (!ni) {
                                    BrowseCand bc = { 0 };
                                    memcpy(bc.id.b, DatumGetPointer(di), sizeof(lp_id));
                                    bc.tier = DatumGetInt16(dt);
                                    bc.mask = DatumGetInt64(dm);
                                    bc.match_kind = BROWSE_KIND_MASK;
                                    lp_push(&pool, bc);
                                }
                            }
                        }
                        ExecClearTuple(slot);
                    }
                    ExecDropSingleTupleTableSlot(slot);
                    table_endscan(tscan);
                }
                table_close(part, NoLock);
            }

            if (pool.n > 0) {
                lp_id *pids = palloc(sizeof(lp_id) * pool.n);
                for (size_t i = 0; i < pool.n; i++) pids[i] = pool.v[i].id;
                lp_pg_lookup_standings(pids, pool.n, st_map);
                pfree(pids);
                for (size_t i = 0; i < pool.n; i++) {
                    const PgStandingEntry *se = lp_idmap_lookup(st_map, &pool.v[i].id);
                    if (se && se->present) {
                        pool.v[i].has_standing = true;
                        pool.v[i].rating = se->rating;
                        pool.v[i].deviation = se->deviation;
                        pool.v[i].confidence = standing_confidence(se->rating, se->deviation);
                        pool.v[i].matches = se->matches;
                    }
                }
                if (pool.n > 1) lp_sort(pool.v, pool.n, sizeof(BrowseCand), cmp_browse_cand);

                size_t start = Min((size_t)off, pool.n);
                size_t stop = Min(need, pool.n);
                if (stop > start) {
                    lp_id *lids = palloc(sizeof(lp_id) * (stop - start));
                    for (size_t i = start; i < stop; i++) lids[i - start] = pool.v[i].id;
                    lp_pg_realize_labels_map(lids, stop - start, max_text, false, bctx, label_map);
                    pfree(lids);

                    for (size_t i = start; i < stop; i++) {
                        CHECK_FOR_INTERRUPTS();
                        const BrowseCand *bc = &pool.v[i];
                        const PgLabelEntry *le = lp_idmap_lookup(label_map, &bc->id);
                        char hex[33];
                        text *t_lbl = NULL;
                        if (le) t_lbl = cstring_to_text_with_len(le->text, le->len);
                        else { lp_id_hex(&bc->id, hex); t_lbl = cstring_to_text_with_len(hex, 32); }
                        text *t_kind = cstring_to_text(browse_kind_name(bc->match_kind));

                        Datum v[10] = {
                            lp_id_bytea_datum(&bc->id),
                            Int16GetDatum(bc->tier),
                            Int64GetDatum(bc->mask),
                            bc->has_standing ? Float8GetDatum(bc->rating) : (Datum)0,
                            bc->has_standing ? Float8GetDatum(bc->deviation) : (Datum)0,
                            bc->has_standing ? Float8GetDatum(bc->confidence) : (Datum)0,
                            Int64GetDatum(bc->has_standing ? bc->matches : 0),
                            PointerGetDatum(t_lbl),
                            PointerGetDatum(t_kind),
                            Int64GetDatum(total_count)
                        };
                        bool nl[10] = { false, false, false, !bc->has_standing, !bc->has_standing, !bc->has_standing, false, false, false, false };
                        set_put(fcinfo, ts, td, v, nl);
                        pfree(DatumGetPointer(v[0]));
                        pfree(t_lbl);
                        pfree(t_kind);
                    }
                }
            }
            lp_vec_free(&pool);
        } else {
            /* ---- Path 3: consensus_standing 16-way merge + indexed entity partition fallback ---- */
            lp_vec(BrowseCand) page = { 0 };
            lp_idmap *seen_ids = lp_idmap_new();

            standing_merge_open(&merge, 1);
            size_t scan_budget = Max((size_t)4096, need * 32);
            size_t scanned = 0;
            const size_t BATCH = 256;
            StandingRow *batch = palloc(sizeof(StandingRow) * BATCH);
            lp_id *batch_ids = palloc(sizeof(lp_id) * BATCH);

            while (page.n < need && scanned < scan_budget) {
                CHECK_FOR_INTERRUPTS();
                size_t nb = 0;
                while (nb < BATCH && scanned < scan_budget && standing_merge_next(&merge, &batch[nb])) {
                    batch_ids[nb] = batch[nb].claim;
                    nb++;
                    scanned++;
                }
                if (nb == 0) break;
                lp_pg_lookup_entity_meta(batch_ids, nb, meta_map);
                for (size_t i = 0; i < nb && page.n < need; i++) {
                    const PgEntityMeta *em = lp_idmap_lookup(meta_map, &batch[i].claim);
                    if (!em || !em->present) continue;
                    if (!browse_tier_ok(&filter, em->tier)) continue;
                    bool fresh = false;
                    lp_idmap_put(seen_ids, &batch[i].claim, &fresh);
                    if (!fresh) continue;
                    BrowseCand bc = {
                        .id = batch[i].claim,
                        .tier = em->tier,
                        .mask = em->mask,
                        .has_standing = true,
                        .rating = batch[i].rating,
                        .deviation = batch[i].deviation,
                        .confidence = batch[i].confidence,
                        .matches = batch[i].matches,
                        .match_kind = BROWSE_KIND_STANDING
                    };
                    lp_push(&page, bc);
                }
            }
            pfree(batch_ids);
            pfree(batch);
            standing_merge_close(&merge);

            /* If standing rows didn't fill need (e.g., tier 0/1 entities without consensus),
             * fill remaining rows via indexed B-tree scan on matching entity partitions. */
            if (page.n < need) {
                const Oid *parts = NULL;
                const int16 *min_t = NULL, *max_t = NULL;
                int nparts = lp_pg_canonical_partitions("entity", &parts, NULL, NULL, NULL);
                (void)lp_pg_canonical_tier_bounds("entity", &min_t, &max_t);
                Snapshot snap = GetActiveSnapshot();

                for (int p = 0; p < nparts && page.n < need; p++) {
                    CHECK_FOR_INTERRUPTS();
                    if (min_t && max_t && !browse_part_tier_ok(&filter, min_t[p], max_t[p]))
                        continue;
                    Relation part = table_open(parts[p], AccessShareLock);
                    if (RelationGetNumberOfBlocks(part) == 0) {
                        table_close(part, NoLock);
                        continue;
                    }
                    AttrNumber a_id = get_attnum(parts[p], "id");
                    AttrNumber a_tier = get_attnum(parts[p], "tier");
                    AttrNumber a_mask = get_attnum(parts[p], "mask");
                    Oid btree_oid = find_entity_browse_btree(part, a_tier, a_id);
                    TupleTableSlot *slot = table_slot_create(part, NULL);

                    if (OidIsValid(btree_oid)) {
                        Relation idx = index_open(btree_oid, AccessShareLock);
                        IndexScanDesc iscan = index_beginscan(part, idx, snap, NULL, 0, 0);
                        index_rescan(iscan, NULL, 0, NULL, 0);
                        while (page.n < need && index_getnext_slot(iscan, ForwardScanDirection, slot)) {
                            CHECK_FOR_INTERRUPTS();
                            bool ni = true, nt = true, nm = true;
                            Datum dt = slot_getattr(slot, a_tier, &nt);
                            if (!nt && browse_tier_ok(&filter, DatumGetInt16(dt))) {
                                Datum di = slot_getattr(slot, a_id, &ni);
                                Datum dm = slot_getattr(slot, a_mask, &nm);
                                if (!ni) {
                                    lp_id id;
                                    memcpy(id.b, DatumGetPointer(di), sizeof(lp_id));
                                    bool fresh = false;
                                    lp_idmap_put(seen_ids, &id, &fresh);
                                    if (fresh) {
                                        BrowseCand bc = {
                                            .id = id,
                                            .tier = DatumGetInt16(dt),
                                            .mask = nm ? 0 : DatumGetInt64(dm),
                                            .match_kind = BROWSE_KIND_ENTITY
                                        };
                                        lp_push(&page, bc);
                                    }
                                }
                            }
                            ExecClearTuple(slot);
                        }
                        index_endscan(iscan);
                        index_close(idx, NoLock);
                    } else {
                        TableScanDesc tscan = table_beginscan(part, snap, 0, NULL);
                        while (page.n < need && table_scan_getnextslot(tscan, ForwardScanDirection, slot)) {
                            CHECK_FOR_INTERRUPTS();
                            bool ni = true, nt = true, nm = true;
                            Datum dt = slot_getattr(slot, a_tier, &nt);
                            if (!nt && browse_tier_ok(&filter, DatumGetInt16(dt))) {
                                Datum di = slot_getattr(slot, a_id, &ni);
                                Datum dm = slot_getattr(slot, a_mask, &nm);
                                if (!ni) {
                                    lp_id id;
                                    memcpy(id.b, DatumGetPointer(di), sizeof(lp_id));
                                    bool fresh = false;
                                    lp_idmap_put(seen_ids, &id, &fresh);
                                    if (fresh) {
                                        BrowseCand bc = {
                                            .id = id,
                                            .tier = DatumGetInt16(dt),
                                            .mask = nm ? 0 : DatumGetInt64(dm),
                                            .match_kind = BROWSE_KIND_ENTITY
                                        };
                                        lp_push(&page, bc);
                                    }
                                }
                            }
                            ExecClearTuple(slot);
                        }
                        table_endscan(tscan);
                    }
                    ExecDropSingleTupleTableSlot(slot);
                    table_close(part, NoLock);
                }
            }

            total_count = (int64)page.n;
            size_t start = Min((size_t)off, page.n);
            size_t stop = Min(need, page.n);
            if (stop > start) {
                lp_id *lids = palloc(sizeof(lp_id) * (stop - start));
                for (size_t i = start; i < stop; i++) lids[i - start] = page.v[i].id;
                lp_pg_realize_labels_map(lids, stop - start, max_text, false, bctx, label_map);
                pfree(lids);

                for (size_t i = start; i < stop; i++) {
                    CHECK_FOR_INTERRUPTS();
                    const BrowseCand *bc = &page.v[i];
                    const PgLabelEntry *le = lp_idmap_lookup(label_map, &bc->id);
                    char hex[33];
                    text *t_lbl = NULL;
                    if (le) t_lbl = cstring_to_text_with_len(le->text, le->len);
                    else { lp_id_hex(&bc->id, hex); t_lbl = cstring_to_text_with_len(hex, 32); }
                    text *t_kind = cstring_to_text(browse_kind_name(bc->match_kind));

                    Datum v[10] = {
                        lp_id_bytea_datum(&bc->id),
                        Int16GetDatum(bc->tier),
                        Int64GetDatum(bc->mask),
                        bc->has_standing ? Float8GetDatum(bc->rating) : (Datum)0,
                        bc->has_standing ? Float8GetDatum(bc->deviation) : (Datum)0,
                        bc->has_standing ? Float8GetDatum(bc->confidence) : (Datum)0,
                        Int64GetDatum(bc->has_standing ? bc->matches : 0),
                        PointerGetDatum(t_lbl),
                        PointerGetDatum(t_kind),
                        Int64GetDatum(total_count)
                    };
                    bool nl[10] = { false, false, false, !bc->has_standing, !bc->has_standing, !bc->has_standing, false, false, false, false };
                    set_put(fcinfo, ts, td, v, nl);
                    pfree(DatumGetPointer(v[0]));
                    pfree(t_lbl);
                    pfree(t_kind);
                }
            }
            lp_idmap_free(seen_ids);
            lp_vec_free(&page);
        }
    }
    PG_FINALLY();
    {
        standing_merge_close(&merge);
        lp_idmap_free(label_map);
        lp_idmap_free(st_map);
        lp_idmap_free(meta_map);
        lp_idmap_free(cand_map);
        MemoryContextDelete(bctx);
        if (prefix) pfree(prefix);
        if (filter.mask_arr) pfree(filter.mask_arr);
    }
    PG_END_TRY();
    PG_RETURN_NULL();
}
