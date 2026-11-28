/* Zero-SPI Native C lexical & taxonomy read operators retiring C# inline SQL CTE pyramids:
 *   - laplace_lexical_resolve: 1-hop direct + 2-hop sense/synset-bridged claim resolution
 *   - laplace_taxonomy_tree:   multi-depth upward & downward taxonomy traversal
 */
#include "pg_core.h"
#include "geo.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include <math.h>
#include <string.h>

/* ---------------------------------------------------------------- shared helpers */

static inline double lex_confidence(double rating, double deviation)
{
    lp_rating r = { rating, deviation, LP_GLICKO_VOLATILITY };
    return lp_confidence(&r, 2.0);
}

static inline Datum lex_id_bytea(const lp_id *id)
{
    return PointerGetDatum(bytes_datum(id->b, sizeof(lp_id)));
}

static void parse_bytea_id(bytea *b, const char *fn_name, const char *arg_name, lp_id *out)
{
    size_t len = (size_t)VARSIZE_ANY_EXHDR(b);
    if (len != sizeof(lp_id))
        ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                        errmsg("%s: %s must be 16 bytes (got %zu)", fn_name, arg_name, len)));
    memcpy(out->b, VARDATA_ANY(b), sizeof(lp_id));
}

/* Parse a 1-D text[] of relation names into deduplicated BLAKE3 text IDs.
 * Fail loud on >1-D arrays, NULL elements, or empty/unhashable names. */
static lp_id *parse_relation_names(ArrayType *arr, const char *fn_name, const char *arg_name, int *out_n)
{
    if (ARR_NDIM(arr) > 1)
        ereport(ERROR, (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                        errmsg("%s: %s must be a 1-D text[]", fn_name, arg_name)));
    Datum *elems = NULL;
    bool *nulls = NULL;
    int n = 0;
    deconstruct_array(arr, TEXTOID, -1, false, TYPALIGN_INT, &elems, &nulls, &n);
    if (n <= 0) {
        if (elems) pfree(elems);
        if (nulls) pfree(nulls);
        *out_n = 0;
        return NULL;
    }
    lp_text *tx = lp_pg_text_context();
    lp_id *ids = palloc(sizeof(lp_id) * (Size)n);
    for (int i = 0; i < n; i++) {
        if (nulls[i]) {
            pfree(ids); pfree(elems); pfree(nulls);
            ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                            errmsg("%s: %s cannot contain NULL elements", fn_name, arg_name)));
        }
        text *t = DatumGetTextPP(elems[i]);
        int tlen = VARSIZE_ANY_EXHDR(t);
        if (tlen <= 0 || !lp_text_id(tx, VARDATA_ANY(t), (size_t)tlen, &ids[i])) {
            pfree(ids); pfree(elems); pfree(nulls);
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("%s: %s contains an empty or invalid UTF-8 relation name", fn_name, arg_name)));
        }
    }
    if (elems) pfree(elems);
    if (nulls) pfree(nulls);
    *out_n = (int)lp_ids_unique(ids, (size_t)n);
    return ids;
}

static text *label_text_or_hex(lp_idmap *label_map, const lp_id *id)
{
    const PgLabelEntry *le = lp_idmap_lookup(label_map, id);
    if (le)
        return cstring_to_text_with_len(le->text, le->len);
    char hex[33];
    lp_id_hex(id, hex);
    return cstring_to_text_with_len(hex, 32);
}

static int lex_utf8_trunc(const char *s, int len, int32 max_bytes)
{
    if (max_bytes <= 0 || len <= max_bytes) return len;
    int cut = (int)max_bytes;
    while (cut > 0 && (((unsigned char)s[cut] & 0xC0u) == 0x80u)) cut--;
    return cut;
}

/* ---------------------------------------------------------------- shared sense/synset bridge discovery */

typedef struct {
    lp_id bridge_id;
    lp_id via_claim;
    double confidence;
    int64 matches;
} BridgeAnchor;

typedef struct {
    lp_idmap *by_id; /* lp_id -> BridgeAnchor */
} BridgeMapCollect;

static void bridge_record(lp_idmap *by_id, const lp_id *target, const lp_id *via_claim,
                          double conf, int64 matches)
{
    bool fresh = false;
    BridgeAnchor *ba = lp_idmap_get(by_id, target, &fresh);
    if (fresh || conf > ba->confidence ||
        (conf == ba->confidence && matches > ba->matches) ||
        (conf == ba->confidence && matches == ba->matches &&
         memcmp(via_claim->b, ba->via_claim.b, sizeof(lp_id)) < 0)) {
        ba->bridge_id = *target;
        ba->via_claim = *via_claim;
        ba->confidence = conf;
        ba->matches = matches;
    }
}

static void bridge_sense_hit(const PgClaimHit *hit, void *context)
{
    BridgeMapCollect *c = context;
    if (!hit->path) return;
    Geo g = geo_of(hit->path);
    lp_id part[4];
    if (lp_path_len(g.p) != 3 || lp_path_expand(g.p, part, 4) != 3) return;
    bridge_record(c->by_id, &part[2], &hit->entity,
                  lex_confidence(hit->rating, hit->deviation), hit->matches);
}

static void bridge_member2_hit(const PgClaimHit *hit, void *context)
{
    BridgeMapCollect *c = context;
    if (!hit->path) return;
    Geo g = geo_of(hit->path);
    lp_id part[4];
    if (lp_path_len(g.p) != 2 || lp_path_expand(g.p, part, 4) != 2) return;
    bridge_record(c->by_id, &part[0], &hit->entity,
                  lex_confidence(hit->rating, hit->deviation), hit->matches);
}

typedef struct {
    lp_idmap *sense_map;  /* sense_id -> BridgeAnchor */
    lp_idmap *out_map;    /* synset_id -> BridgeAnchor */
} SensePhysCollect;

static void sense_phys_visit(const lp_id *id, Datum path, Datum mask, void *context)
{
    (void)mask;
    SensePhysCollect *c = context;
    const BridgeAnchor *src = lp_idmap_lookup(c->sense_map, id);
    if (!src || !path) return;
    Geo g = geo_of(path);
    size_t n = lp_path_len(g.p);
    if (n < 3 || n > 64) return;
    lp_id part[64];
    if (lp_path_expand(g.p, part, 64) != n) return;
    bridge_record(c->out_map, &part[n - 1], &src->via_claim, src->confidence, src->matches);
}

typedef struct {
    const lp_id *senses;
    int nsenses;
    lp_idmap *sense_map;
    lp_idmap *out_map;
} SenseRefCollect;

static void sense_ref_hit(const PgClaimHit *hit, void *context)
{
    SenseRefCollect *c = context;
    int idx = (int)hit->i - 1;
    if (idx < 0 || idx >= c->nsenses || !hit->path) return;
    Geo g = geo_of(hit->path);
    lp_id part[4];
    if (lp_path_len(g.p) != 3 || lp_path_expand(g.p, part, 4) != 3) return;
    bridge_record(c->out_map, &part[2], &hit->entity,
                  lex_confidence(hit->rating, hit->deviation), hit->matches);
}

/* Discover sense & synset bridge nodes reachable from `term` via:
 *  1) [term, 'lexical sense' | 'sense' | 'reference', mid]
 *  2) physicality of `mid` when 3+ parts ([lemma, lang, synset]) -> synset
 *  3) [mid, 'reference', synset] -> synset
 *  4) binary membership [synset, term] (when include_binary is true)
 * `term` itself is excluded from `out_bridges`. */
static void discover_sense_bridges(const lp_id *term, int64 sense_fan, bool include_binary,
                                   lp_idmap *out_bridges)
{
    lp_text *tx = lp_pg_text_context();
    lp_mask claim_mask = lp_pg_kind_mask("claim");

    lp_id sense_rels[3];
    int nsrels = 0;
    if (lp_text_id(tx, "lexical sense", 13, &sense_rels[nsrels])) nsrels++;
    if (lp_text_id(tx, "sense", 5, &sense_rels[nsrels])) nsrels++;
    if (lp_text_id(tx, "reference", 9, &sense_rels[nsrels])) nsrels++;
    if (nsrels == 0) return;

    lp_id id_reference = { { 0 } };
    bool has_ref = lp_text_id(tx, "reference", 9, &id_reference);

    lp_idmap *senses = lp_idmap_sized(sizeof(BridgeAnchor));
    lp_idmap *all = lp_idmap_sized(sizeof(BridgeAnchor));

    PG_TRY();
    {
        /* 1. Direct sense/reference links from term */
        BridgeMapCollect bc = { .by_id = senses };
        lp_pg_claims_each_direct(term, 1, sense_fan, claim_mask,
                                 NULL, 0, 1, sense_rels, nsrels, 3, NULL, 0,
                                 bridge_sense_hit, &bc);

        size_t ns = lp_idmap_count(senses);
        for (size_t i = 0; i < ns; i++) {
            const BridgeAnchor *ba = lp_idmap_at(senses, i);
            if (!lp_id_eq(&ba->bridge_id, term))
                bridge_record(all, &ba->bridge_id, &ba->via_claim, ba->confidence, ba->matches);
        }

        if (ns > 0) {
            const lp_id *sense_ids = lp_idmap_keys(senses);
            /* 2. Physicality [lemma, lang, synset] unpacking */
            SensePhysCollect spc = { .sense_map = senses, .out_map = all };
            lp_pg_lookup_paths(sense_ids, ns, false, sense_phys_visit, &spc);

            /* 3. [sense, 'reference', synset] claims */
            if (has_ref) {
                SenseRefCollect src = {
                    .senses = sense_ids,
                    .nsenses = (int)ns,
                    .sense_map = senses,
                    .out_map = all
                };
                lp_pg_claims_each_direct(sense_ids, (int)ns, 8, claim_mask,
                                         NULL, 0, 1, &id_reference, 1, 3, NULL, 0,
                                         sense_ref_hit, &src);
            }
        }

        /* 4. Binary synset membership [synset, term] */
        if (include_binary) {
            BridgeMapCollect b2 = { .by_id = all };
            lp_pg_claims_each_direct(term, 1, 16, claim_mask,
                                     NULL, 0, 2, NULL, 0, 2, NULL, 0,
                                     bridge_member2_hit, &b2);
        }

        for (size_t i = 0; i < lp_idmap_count(all); i++) {
            const BridgeAnchor *ba = lp_idmap_at(all, i);
            if (!lp_id_eq(&ba->bridge_id, term))
                bridge_record(out_bridges, &ba->bridge_id, &ba->via_claim, ba->confidence, ba->matches);
        }
    }
    PG_FINALLY();
    {
        lp_idmap_free(all);
        lp_idmap_free(senses);
    }
    PG_END_TRY();
}

/* ---------------------------------------------------------------- 1. laplace_lexical_resolve */

typedef struct {
    int32 hop;
    lp_id subject;
    lp_id relation;
    lp_id target;
    lp_id claim;
    bool has_via;
    lp_id via_claim;
    double rating;
    double deviation;
    double confidence;
    int64 matches;
    int64 witnesses;
} LexRow;

typedef struct {
    lp_idmap *by_claim;      /* claim -> LexRow */
    int32 hop;
    const BridgeAnchor *bridges; /* NULL for hop 1 */
    int nbridges;
} LexCollect;

static void lex_add_row(lp_idmap *by_claim, const LexRow *cand)
{
    bool fresh = false;
    LexRow *dst = lp_idmap_get(by_claim, &cand->claim, &fresh);
    if (fresh || cand->hop < dst->hop ||
        (cand->hop == dst->hop && cand->confidence > dst->confidence) ||
        (cand->hop == dst->hop && cand->confidence == dst->confidence && cand->matches > dst->matches)) {
        *dst = *cand;
    }
}

static void lex_claim_hit(const PgClaimHit *hit, void *context)
{
    LexCollect *c = context;
    if (!hit->path) return;
    Geo g = geo_of(hit->path);
    lp_id part[4];
    if (lp_path_len(g.p) != 3 || lp_path_expand(g.p, part, 4) != 3) return;

    LexRow row = {
        .hop = c->hop,
        .subject = part[0],
        .relation = part[1],
        .target = part[2],
        .claim = hit->entity,
        .has_via = false,
        .rating = hit->rating,
        .deviation = hit->deviation,
        .confidence = lex_confidence(hit->rating, hit->deviation),
        .matches = hit->matches,
        .witnesses = hit->matches
    };
    if (c->bridges) {
        int idx = (int)hit->i - 1;
        if (idx < 0 || idx >= c->nbridges) return;
        row.has_via = true;
        row.via_claim = c->bridges[idx].via_claim;
    }
    lex_add_row(c->by_claim, &row);
}

static int cmp_lex_row(const void *a, const void *b)
{
    const LexRow *x = a, *y = b;
    bool nx = isnan(x->confidence), ny = isnan(y->confidence);
    if (nx != ny) return nx ? 1 : -1;
    if (x->confidence != y->confidence) return x->confidence > y->confidence ? -1 : 1;
    if (x->matches != y->matches) return x->matches > y->matches ? -1 : 1;
    if (x->hop != y->hop) return x->hop < y->hop ? -1 : 1;
    return memcmp(x->claim.b, y->claim.b, sizeof(lp_id));
}

typedef struct {
    lp_id claim;
    lp_id trunk;
} LexAttPair;

static int cmp_lex_att_pair(const void *a, const void *b)
{
    const LexAttPair *x = a, *y = b;
    int c = memcmp(x->claim.b, y->claim.b, sizeof(lp_id));
    return c ? c : memcmp(x->trunk.b, y->trunk.b, sizeof(lp_id));
}

PG_FUNCTION_INFO_V1(laplace_lexical_resolve);
Datum laplace_lexical_resolve(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_lexical_resolve", &td);

    if (PG_ARGISNULL(0)) PG_RETURN_NULL();
    lp_id term;
    parse_bytea_id(PG_GETARG_BYTEA_PP(0), "laplace_lexical_resolve", "term", &term);

    int32 lim = (PG_NARGS() > 2 && !PG_ARGISNULL(2)) ? PG_GETARG_INT32(2) : 20;
    int32 max_text = (PG_NARGS() > 3 && !PG_ARGISNULL(3)) ? PG_GETARG_INT32(3) : 0;
    if (lim < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_lexical_resolve: lim must be >= 0 (got %d)", lim)));
    if (max_text < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_lexical_resolve: max_text must be >= 0 (got %d)", max_text)));

    bool has_rels = (PG_NARGS() > 1 && !PG_ARGISNULL(1));
    int nrels = 0;
    lp_id *rel_ids = has_rels
        ? parse_relation_names(PG_GETARG_ARRAYTYPE_P(1), "laplace_lexical_resolve", "relation_names", &nrels)
        : NULL;
    if (lim == 0 || (has_rels && nrels == 0)) {
        if (rel_ids) pfree(rel_ids);
        PG_RETURN_NULL();
    }

    MemoryContext lctx = AllocSetContextCreate(CurrentMemoryContext, "laplace_lexical_resolve", ALLOCSET_DEFAULT_SIZES);
    lp_idmap *by_claim = lp_idmap_sized(sizeof(LexRow));
    lp_idmap *bridges = lp_idmap_sized(sizeof(BridgeAnchor));
    lp_idmap *witness_counts = lp_idmap_sized(sizeof(int64));
    lp_idmap *label_map = lp_idmap_sized(sizeof(PgLabelEntry));

    PG_TRY();
    {
        lp_mask claim_mask = lp_pg_kind_mask("claim");
        int64 fan1 = Max((int64)lim * 4, (int64)64);

        /* Hop 1: direct [term, rel, obj] claims */
        LexCollect c1 = { .by_claim = by_claim, .hop = 1, .bridges = NULL, .nbridges = 0 };
        lp_pg_claims_each_direct(&term, 1, fan1, claim_mask,
                                 NULL, 0, 1,
                                 has_rels ? rel_ids : NULL, has_rels ? nrels : 0,
                                 3, NULL, 0,
                                 lex_claim_hit, &c1);

        /* Hop 2: sense/synset-bridged [term, sense_rel, sense] -> [sense/synset, rel, obj] */
        int64 sense_fan = Max((int64)lim * 2, (int64)64);
        discover_sense_bridges(&term, sense_fan, true, bridges);

        size_t nb = lp_idmap_count(bridges);
        if (nb > 0) {
            BridgeAnchor *barr = palloc(sizeof(BridgeAnchor) * nb);
            lp_id *bids = palloc(sizeof(lp_id) * nb);
            for (size_t i = 0; i < nb; i++) {
                barr[i] = *(BridgeAnchor *)lp_idmap_at(bridges, i);
                bids[i] = barr[i].bridge_id;
            }
            int64 fan2 = Max((int64)lim * 2, (int64)32);
            LexCollect c2 = { .by_claim = by_claim, .hop = 2, .bridges = barr, .nbridges = (int)nb };
            lp_pg_claims_each_direct(bids, (int)nb, fan2, claim_mask,
                                     NULL, 0, 1,
                                     has_rels ? rel_ids : NULL, has_rels ? nrels : 0,
                                     3, NULL, 0,
                                     lex_claim_hit, &c2);
            pfree(bids);
            pfree(barr);
        }

        size_t total = lp_idmap_count(by_claim);
        if (total > 0) {
            LexRow *rows = palloc(sizeof(LexRow) * total);
            for (size_t i = 0; i < total; i++)
                rows[i] = *(LexRow *)lp_idmap_at(by_claim, i);
            if (total > 1)
                lp_sort(rows, total, sizeof(LexRow), cmp_lex_row);

            size_t emit_n = Min(total, (size_t)lim);

            /* Batch distinct witness counts from attestation PK */
            lp_id *claim_ids = palloc(sizeof(lp_id) * emit_n);
            for (size_t i = 0; i < emit_n; i++) claim_ids[i] = rows[i].claim;
            PgAttrRows att = { 0 };
            lp_pg_read_attestation_pk(claim_ids, (int)emit_n, NULL, &att, NULL);
            if (att.n > 0) {
                LexAttPair *pairs = palloc(sizeof(LexAttPair) * att.n);
                for (size_t i = 0; i < att.n; i++)
                    pairs[i] = (LexAttPair){ att.v[i].claim, att.v[i].trunk };
                if (att.n > 1) lp_sort(pairs, att.n, sizeof(LexAttPair), cmp_lex_att_pair);
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
            pfree(claim_ids);

            /* Batch-realize all subject, relation, and target labels in one pass */
            lp_idmap *lbl_ids = lp_idmap_new();
            for (size_t i = 0; i < emit_n; i++) {
                lp_idmap_put(lbl_ids, &rows[i].subject, NULL);
                lp_idmap_put(lbl_ids, &rows[i].relation, NULL);
                lp_idmap_put(lbl_ids, &rows[i].target, NULL);
            }
            if (lp_idmap_count(lbl_ids) > 0) {
                lp_pg_realize_labels_map(lp_idmap_keys(lbl_ids), lp_idmap_count(lbl_ids),
                                         max_text, false, lctx, label_map);
            }
            lp_idmap_free(lbl_ids);

            for (size_t i = 0; i < emit_n; i++) {
                CHECK_FOR_INTERRUPTS();
                const LexRow *r = &rows[i];
                int64 wits = r->witnesses;
                const int64 *wc = lp_idmap_lookup(witness_counts, &r->claim);
                if (wc && *wc > 0) wits = *wc;

                text *t_subj = label_text_or_hex(label_map, &r->subject);
                text *t_rel  = label_text_or_hex(label_map, &r->relation);
                text *t_tgt  = label_text_or_hex(label_map, &r->target);

                Datum v[14] = {
                    Int32GetDatum(r->hop),
                    lex_id_bytea(&r->subject),
                    PointerGetDatum(t_subj),
                    lex_id_bytea(&r->relation),
                    PointerGetDatum(t_rel),
                    lex_id_bytea(&r->target),
                    PointerGetDatum(t_tgt),
                    lex_id_bytea(&r->claim),
                    r->has_via ? lex_id_bytea(&r->via_claim) : (Datum)0,
                    Float8GetDatum(r->rating),
                    Float8GetDatum(r->deviation),
                    Float8GetDatum(r->confidence),
                    Int64GetDatum(r->matches),
                    Int64GetDatum(wits)
                };
                bool nl[14] = {
                    false, false, false, false, false, false, false, false,
                    !r->has_via, false, false, false, false, false
                };
                set_put(fcinfo, ts, td, v, nl);

                pfree(DatumGetPointer(v[1]));
                pfree(t_subj);
                pfree(DatumGetPointer(v[3]));
                pfree(t_rel);
                pfree(DatumGetPointer(v[5]));
                pfree(t_tgt);
                pfree(DatumGetPointer(v[7]));
                if (!nl[8]) pfree(DatumGetPointer(v[8]));
            }
            pfree(rows);
        }
    }
    PG_FINALLY();
    {
        lp_idmap_free(label_map);
        lp_idmap_free(witness_counts);
        lp_idmap_free(bridges);
        lp_idmap_free(by_claim);
        MemoryContextDelete(lctx);
        if (rel_ids) pfree(rel_ids);
    }
    PG_END_TRY();
    PG_RETURN_NULL();
}

/* ---------------------------------------------------------------- 2. laplace_taxonomy_tree */

typedef struct {
    int32 depth;
    lp_id parent;
    lp_id child;
    lp_id relation;
    lp_id claim;
    double rating;
    double deviation;
    double confidence;
    int64 matches;
} TaxEdge;

typedef lp_vec(TaxEdge) TaxEdgeVec;

typedef struct {
    const lp_id *frontier;
    int nfrontier;
    lp_id root;
    int32 depth;
    bool inverse;       /* false: [u, rel, v] -> child=v; true: [v, rel, u] -> child=v */
    lp_idmap *visited;
    lp_idmap *level_map; /* child -> TaxEdge (best edge at this depth) */
} TaxStepCollect;

static void tax_step_hit(const PgClaimHit *hit, void *context)
{
    TaxStepCollect *c = context;
    int idx = (int)hit->i - 1;
    if (idx < 0 || idx >= c->nfrontier || !hit->path) return;
    Geo g = geo_of(hit->path);
    lp_id part[4];
    if (lp_path_len(g.p) != 3 || lp_path_expand(g.p, part, 4) != 3) return;

    const lp_id *from_id = &c->frontier[idx];
    lp_id child;
    if (!c->inverse) {
        if (!lp_id_eq(&part[0], from_id)) return;
        child = part[2];
    } else {
        if (!lp_id_eq(&part[2], from_id)) return;
        child = part[0];
    }
    if (lp_idmap_find(c->visited, &child) >= 0) return;

    double conf = lex_confidence(hit->rating, hit->deviation);
    lp_id parent_out = (c->depth == 1) ? c->root : *from_id;

    bool fresh = false;
    TaxEdge *e = lp_idmap_get(c->level_map, &child, &fresh);
    if (fresh || conf > e->confidence ||
        (conf == e->confidence && hit->matches > e->matches) ||
        (conf == e->confidence && hit->matches == e->matches &&
         memcmp(hit->entity.b, e->claim.b, sizeof(lp_id)) < 0)) {
        e->depth = c->depth;
        e->parent = parent_out;
        e->child = child;
        e->relation = part[1];
        e->claim = hit->entity;
        e->rating = hit->rating;
        e->deviation = hit->deviation;
        e->confidence = conf;
        e->matches = hit->matches;
    }
}

static int cmp_tax_edge(const void *a, const void *b)
{
    const TaxEdge *x = a, *y = b;
    if (x->depth != y->depth) return x->depth < y->depth ? -1 : 1;
    bool nx = isnan(x->confidence), ny = isnan(y->confidence);
    if (nx != ny) return nx ? 1 : -1;
    if (x->confidence != y->confidence) return x->confidence > y->confidence ? -1 : 1;
    if (x->matches != y->matches) return x->matches > y->matches ? -1 : 1;
    return memcmp(x->child.b, y->child.b, sizeof(lp_id));
}

static void run_taxonomy_bfs(const lp_id *root, const lp_id *anchors, size_t nanchors,
                             const lp_id *fwd_rels, int nfwd,
                             const lp_id *inv_rels, int ninv,
                             int32 max_depth, size_t max_collect,
                             TaxEdgeVec *out_edges)
{
    if (max_collect == 0 || (nfwd == 0 && ninv == 0) || nanchors == 0) return;
    lp_mask claim_mask = lp_pg_kind_mask("claim");
    lp_idmap *visited = lp_idmap_new();
    lp_idmap *level_map = lp_idmap_sized(sizeof(TaxEdge));
    lp_vec(lp_id) frontier = { 0 };
    lp_vec(lp_id) next_frontier = { 0 };

    PG_TRY();
    {
        for (size_t i = 0; i < nanchors; i++) {
            bool fresh = false;
            lp_idmap_put(visited, &anchors[i], &fresh);
            if (fresh) lp_push(&frontier, anchors[i]);
        }

        int64 fan = Max((int64)max_collect * 2, (int64)32);
        for (int32 d = 1; d <= max_depth && frontier.n > 0 && out_edges->n < max_collect; d++) {
            CHECK_FOR_INTERRUPTS();
            lp_idmap_clear(level_map);

            if (nfwd > 0) {
                TaxStepCollect sc = {
                    .frontier = frontier.v,
                    .nfrontier = (int)frontier.n,
                    .root = *root,
                    .depth = d,
                    .inverse = false,
                    .visited = visited,
                    .level_map = level_map
                };
                lp_pg_claims_each_direct(frontier.v, (int)frontier.n, fan, claim_mask,
                                         NULL, 0, 1, fwd_rels, nfwd, 3, NULL, 0,
                                         tax_step_hit, &sc);
            }
            if (ninv > 0) {
                TaxStepCollect sc = {
                    .frontier = frontier.v,
                    .nfrontier = (int)frontier.n,
                    .root = *root,
                    .depth = d,
                    .inverse = true,
                    .visited = visited,
                    .level_map = level_map
                };
                lp_pg_claims_each_direct(frontier.v, (int)frontier.n, fan, claim_mask,
                                         NULL, 0, 2, inv_rels, ninv, 3, NULL, 0,
                                         tax_step_hit, &sc);
            }

            size_t nl = lp_idmap_count(level_map);
            if (nl == 0) break;

            TaxEdge *arr = palloc(sizeof(TaxEdge) * nl);
            for (size_t i = 0; i < nl; i++)
                arr[i] = *(TaxEdge *)lp_idmap_at(level_map, i);
            if (nl > 1)
                lp_sort(arr, nl, sizeof(TaxEdge), cmp_tax_edge);

            next_frontier.n = 0;
            size_t room = max_collect - out_edges->n;
            size_t take = Min(nl, room);
            for (size_t i = 0; i < take; i++) {
                lp_idmap_put(visited, &arr[i].child, NULL);
                lp_push(out_edges, arr[i]);
                lp_push(&next_frontier, arr[i].child);
            }
            pfree(arr);

            frontier.n = 0;
            for (size_t i = 0; i < next_frontier.n; i++)
                lp_push(&frontier, next_frontier.v[i]);
        }
    }
    PG_FINALLY();
    {
        lp_vec_free(&next_frontier);
        lp_vec_free(&frontier);
        lp_idmap_free(level_map);
        lp_idmap_free(visited);
    }
    PG_END_TRY();
}

/* Optional WordNet ILI member-label enrichment (e.g., i46360 -> "dog, domestic dog") */
static bool is_ili_code_str(const char *s, int len)
{
    if (len < 2 || s[0] != 'i') return false;
    for (int i = 1; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

static bool is_clean_member_label(const char *s, int len)
{
    if (len <= 0 || len > 64) return false;
    if (is_ili_code_str(s, len)) return false;
    if (len >= 8 && memcmp(s, "I hold \"", 8) == 0) return false;
    if ((len > 5 && memcmp(s, "noun.", 5) == 0) ||
        (len > 5 && memcmp(s, "verb.", 5) == 0) ||
        (len > 4 && memcmp(s, "adj.", 4) == 0) ||
        (len > 4 && memcmp(s, "adv.", 4) == 0))
        return false;
    static const char *const upos[] = {
        "NOUN", "VERB", "ADJ", "ADV", "PROPN", "PRON", "DET", "ADP",
        "CONJ", "SCONJ", "CCONJ", "NUM", "PART", "INTJ", "AUX", "SYM", "PUNCT", "X"
    };
    for (size_t i = 0; i < lengthof(upos); i++) {
        if ((int)strlen(upos[i]) == len && memcmp(s, upos[i], (size_t)len) == 0)
            return false;
    }
    return true;
}

typedef lp_vec(lp_id) LexIdVec;

typedef struct {
    const lp_id *ili_ids;
    int nili;
    LexIdVec *members; /* array of nili vectors */
    lp_idmap *all_member_ids;
} IliMemberCollect;

static void ili_member_hit(const PgClaimHit *hit, void *context)
{
    IliMemberCollect *c = context;
    int idx = (int)hit->i - 1;
    if (idx < 0 || idx >= c->nili || !hit->path) return;
    Geo g = geo_of(hit->path);
    lp_id part[4];
    if (lp_path_len(g.p) != 2 || lp_path_expand(g.p, part, 4) != 2) return;
    if (!lp_id_eq(&part[0], &c->ili_ids[idx])) return;
    if (c->members[idx].n >= 12) return;
    for (size_t k = 0; k < c->members[idx].n; k++) {
        if (lp_id_eq(&c->members[idx].v[k], &part[1])) return;
    }
    lp_push(&c->members[idx], part[1]);
    lp_idmap_put(c->all_member_ids, &part[1], NULL);
}

static void enrich_ili_labels(lp_idmap *label_map, int32 max_text, MemoryContext ctx)
{
    lp_vec(lp_id) ili_ids = { 0 };
    for (size_t i = 0; i < lp_idmap_count(label_map); i++) {
        const PgLabelEntry *le = lp_idmap_at(label_map, i);
        if (le && !le->is_hex && is_ili_code_str(le->text, le->len))
            lp_push(&ili_ids, *lp_idmap_key(label_map, i));
    }
    if (ili_ids.n == 0) {
        lp_vec_free(&ili_ids);
        return;
    }

    LexIdVec *members = palloc0(sizeof(LexIdVec) * ili_ids.n);
    lp_idmap *all_mids = lp_idmap_new();
    lp_idmap *m_labels = lp_idmap_sized(sizeof(PgLabelEntry));

    PG_TRY();
    {
        IliMemberCollect mc = {
            .ili_ids = ili_ids.v,
            .nili = (int)ili_ids.n,
            .members = members,
            .all_member_ids = all_mids
        };
        lp_pg_claims_each_direct(ili_ids.v, (int)ili_ids.n, 12, lp_pg_kind_mask("claim"),
                                 NULL, 0, 1, NULL, 0, 2, NULL, 0,
                                 ili_member_hit, &mc);
        if (lp_idmap_count(all_mids) > 0) {
            lp_pg_realize_labels_map(lp_idmap_keys(all_mids), lp_idmap_count(all_mids),
                                     64, false, ctx, m_labels);
            for (size_t i = 0; i < ili_ids.n; i++) {
                if (members[i].n == 0) continue;
                StringInfoData joined;
                initStringInfo(&joined);
                for (size_t k = 0; k < members[i].n; k++) {
                    const PgLabelEntry *ml = lp_idmap_lookup(m_labels, &members[i].v[k]);
                    if (!ml || ml->is_hex || !is_clean_member_label(ml->text, ml->len)) continue;
                    if (joined.len > 0) appendStringInfoString(&joined, ", ");
                    appendBinaryStringInfo(&joined, ml->text, ml->len);
                }
                if (joined.len > 0) {
                    int cut = lex_utf8_trunc(joined.data, joined.len, max_text);
                    char *copy = MemoryContextAlloc(ctx, (Size)cut + 1);
                    if (cut > 0) memcpy(copy, joined.data, (size_t)cut);
                    copy[cut] = '\0';
                    PgLabelEntry *dst = lp_idmap_lookup(label_map, &ili_ids.v[i]);
                    if (dst) {
                        dst->text = copy;
                        dst->len = cut;
                        dst->is_hex = false;
                    }
                }
                pfree(joined.data);
            }
        }
    }
    PG_FINALLY();
    {
        for (size_t i = 0; i < ili_ids.n; i++) lp_vec_free(&members[i]);
        pfree(members);
        lp_idmap_free(m_labels);
        lp_idmap_free(all_mids);
        lp_vec_free(&ili_ids);
    }
    PG_END_TRY();
}

static void emit_tax_row(FunctionCallInfo fcinfo, Tuplestorestate *ts, TupleDesc td,
                         const char *dir, int32 depth,
                         const lp_id *parent, const lp_id *child,
                         const lp_id *relation, const lp_id *claim,
                         bool has_standing, double rating, double deviation,
                         double confidence, int64 matches, lp_idmap *label_map)
{
    text *t_dir = cstring_to_text(dir);
    text *t_child = label_text_or_hex(label_map, child);
    text *t_rel = relation ? label_text_or_hex(label_map, relation) : NULL;

    Datum v[12] = {
        PointerGetDatum(t_dir),
        Int32GetDatum(depth),
        parent ? lex_id_bytea(parent) : (Datum)0,
        lex_id_bytea(child),
        PointerGetDatum(t_child),
        relation ? lex_id_bytea(relation) : (Datum)0,
        t_rel ? PointerGetDatum(t_rel) : (Datum)0,
        claim ? lex_id_bytea(claim) : (Datum)0,
        has_standing ? Float8GetDatum(rating) : (Datum)0,
        has_standing ? Float8GetDatum(deviation) : (Datum)0,
        has_standing ? Float8GetDatum(confidence) : (Datum)0,
        has_standing ? Int64GetDatum(matches) : (Datum)0
    };
    bool nl[12] = {
        false, false, parent == NULL, false, false,
        relation == NULL, t_rel == NULL, claim == NULL,
        !has_standing, !has_standing, !has_standing, !has_standing
    };
    set_put(fcinfo, ts, td, v, nl);

    pfree(t_dir);
    if (!nl[2]) pfree(DatumGetPointer(v[2]));
    pfree(DatumGetPointer(v[3]));
    pfree(t_child);
    if (!nl[5]) pfree(DatumGetPointer(v[5]));
    if (t_rel) pfree(t_rel);
    if (!nl[7]) pfree(DatumGetPointer(v[7]));
}

PG_FUNCTION_INFO_V1(laplace_taxonomy_tree);
Datum laplace_taxonomy_tree(PG_FUNCTION_ARGS)
{
    TupleDesc td;
    Tuplestorestate *ts = set_begin(fcinfo, "laplace_taxonomy_tree", &td);

    if (PG_ARGISNULL(0)) PG_RETURN_NULL();
    lp_id root;
    parse_bytea_id(PG_GETARG_BYTEA_PP(0), "laplace_taxonomy_tree", "root", &root);

    int32 max_depth = (PG_NARGS() > 3 && !PG_ARGISNULL(3)) ? PG_GETARG_INT32(3) : 3;
    int32 max_nodes = (PG_NARGS() > 4 && !PG_ARGISNULL(4)) ? PG_GETARG_INT32(4) : 64;
    int32 max_text = (PG_NARGS() > 5 && !PG_ARGISNULL(5)) ? PG_GETARG_INT32(5) : 160;

    if (max_depth < 1 || max_depth > 6)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_taxonomy_tree: max_depth must be between 1 and 6 (got %d)", max_depth)));
    if (max_nodes < 1)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_taxonomy_tree: max_nodes must be >= 1 (got %d)", max_nodes)));
    if (max_text < 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("laplace_taxonomy_tree: max_text must be >= 0 (got %d)", max_text)));

    int n_up = 0, n_down = 0;
    lp_id *up_ids = (PG_NARGS() > 1 && !PG_ARGISNULL(1))
        ? parse_relation_names(PG_GETARG_ARRAYTYPE_P(1), "laplace_taxonomy_tree", "up_relations", &n_up)
        : NULL;
    lp_id *down_ids = (PG_NARGS() > 2 && !PG_ARGISNULL(2))
        ? parse_relation_names(PG_GETARG_ARRAYTYPE_P(2), "laplace_taxonomy_tree", "down_relations", &n_down)
        : NULL;

    MemoryContext tctx = AllocSetContextCreate(CurrentMemoryContext, "laplace_taxonomy_tree", ALLOCSET_DEFAULT_SIZES);
    lp_idmap *bridges = lp_idmap_sized(sizeof(BridgeAnchor));
    lp_idmap *label_map = lp_idmap_sized(sizeof(PgLabelEntry));
    lp_vec(lp_id) anchors = { 0 };
    TaxEdgeVec up_edges = { 0 };
    TaxEdgeVec down_edges = { 0 };

    PG_TRY();
    {
        /* Build anchor set: root + any sense/synset bridges of root */
        lp_push(&anchors, root);
        discover_sense_bridges(&root, 16, false, bridges);
        for (size_t i = 0; i < lp_idmap_count(bridges) && anchors.n < 16; i++) {
            const BridgeAnchor *ba = lp_idmap_at(bridges, i);
            lp_push(&anchors, ba->bridge_id);
        }

        size_t budget = (size_t)(max_nodes - 1); /* reserve 1 slot for 'self' */
        if (budget > 0) {
            if (n_up > 0) {
                run_taxonomy_bfs(&root, anchors.v, anchors.n,
                                 up_ids, n_up, NULL, 0,
                                 max_depth, budget, &up_edges);
            }
            if (n_down > 0) {
                run_taxonomy_bfs(&root, anchors.v, anchors.n,
                                 down_ids, n_down, up_ids, n_up,
                                 max_depth, budget, &down_edges);
            }
        }

        /* Balance up & down budgets so total emitted rows (1 + keep_up + keep_down) <= max_nodes */
        size_t keep_up = up_edges.n;
        size_t keep_down = down_edges.n;
        if (keep_up + keep_down > budget) {
            size_t half_up = (budget + 1) / 2;
            size_t half_down = budget - half_up;
            if (keep_up < half_up) {
                keep_down = Min(keep_down, budget - keep_up);
            } else if (keep_down < half_down) {
                keep_up = Min(keep_up, budget - keep_down);
            } else {
                keep_up = half_up;
                keep_down = half_down;
            }
        }

        /* Batch-realize all labels (root, children, relations) in one pass */
        lp_idmap *lbl_ids = lp_idmap_new();
        lp_idmap_put(lbl_ids, &root, NULL);
        for (size_t i = 0; i < keep_up; i++) {
            lp_idmap_put(lbl_ids, &up_edges.v[i].child, NULL);
            lp_idmap_put(lbl_ids, &up_edges.v[i].relation, NULL);
        }
        for (size_t i = 0; i < keep_down; i++) {
            lp_idmap_put(lbl_ids, &down_edges.v[i].child, NULL);
            lp_idmap_put(lbl_ids, &down_edges.v[i].relation, NULL);
        }
        lp_pg_realize_labels_map(lp_idmap_keys(lbl_ids), lp_idmap_count(lbl_ids),
                                 max_text, false, tctx, label_map);
        lp_idmap_free(lbl_ids);

        /* Enrich any WordNet ILI synset labels (i12345) via batched 2-part member lookup */
        enrich_ili_labels(label_map, max_text, tctx);

        /* Emit 'self', then 'up', then 'down' */
        emit_tax_row(fcinfo, ts, td, "self", 0, NULL, &root, NULL, NULL,
                     false, 0.0, 0.0, 0.0, 0, label_map);

        for (size_t i = 0; i < keep_up; i++) {
            CHECK_FOR_INTERRUPTS();
            const TaxEdge *e = &up_edges.v[i];
            emit_tax_row(fcinfo, ts, td, "up", e->depth, &e->parent, &e->child,
                         &e->relation, &e->claim, true,
                         e->rating, e->deviation, e->confidence, e->matches, label_map);
        }
        for (size_t i = 0; i < keep_down; i++) {
            CHECK_FOR_INTERRUPTS();
            const TaxEdge *e = &down_edges.v[i];
            emit_tax_row(fcinfo, ts, td, "down", e->depth, &e->parent, &e->child,
                         &e->relation, &e->claim, true,
                         e->rating, e->deviation, e->confidence, e->matches, label_map);
        }
    }
    PG_FINALLY();
    {
        lp_vec_free(&down_edges);
        lp_vec_free(&up_edges);
        lp_vec_free(&anchors);
        lp_idmap_free(label_map);
        lp_idmap_free(bridges);
        MemoryContextDelete(tctx);
        if (up_ids) pfree(up_ids);
        if (down_ids) pfree(down_ids);
    }
    PG_END_TRY();
    PG_RETURN_NULL();
}
