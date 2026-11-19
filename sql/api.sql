-- Current versioned Native API: one declaration for install, upgrade and repair.
-- Planner support and operator selectivity hooks must come before functions and operators that reference them.
CREATE OR REPLACE FUNCTION @extschema@.laplace_srf_support(internal) RETURNS internal
    LANGUAGE c IMMUTABLE STRICT PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_srf_support';

CREATE OR REPLACE FUNCTION @extschema@.laplace_path_sel(internal, oid, internal, integer) RETURNS double precision
    LANGUAGE c STABLE STRICT PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_path_sel';

CREATE OR REPLACE FUNCTION @extschema@.laplace_path_joinsel(internal, oid, internal, smallint, internal) RETURNS double precision
    LANGUAGE c STABLE STRICT PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_path_joinsel';

CREATE OR REPLACE FUNCTION @extschema@.laplace_mask_sel(internal, oid, internal, integer) RETURNS double precision
    LANGUAGE c STABLE STRICT PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_mask_sel';

CREATE OR REPLACE FUNCTION @extschema@.laplace_mask_joinsel(internal, oid, internal, smallint, internal) RETURNS double precision
    LANGUAGE c STABLE STRICT PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_mask_joinsel';

CREATE OR REPLACE FUNCTION @extschema@.blake3_from_bytea(bytea) RETURNS @extschema@.blake3
    AS 'MODULE_PATHNAME', 'blake3_from_bytea' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION @extschema@.laplace_path_has_roles(path @extschema:postgis@.geometry, roles smallint) RETURNS boolean
    AS 'MODULE_PATHNAME', 'laplace_path_has_roles' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION @extschema@.laplace_testimony(claims @extschema@.blake3[], sources @extschema@.blake3[] DEFAULT NULL,
    OUT claim @extschema@.blake3, OUT source @extschema@.blake3, OUT context @extschema@.blake3,
    OUT ordinal bigint, OUT outcome smallint, OUT occurrences bigint, OUT tokens bigint,
    OUT score double precision, OUT "position" integer, OUT trust double precision,
    OUT dependence_root @extschema@.blake3, OUT calculation boolean) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 256 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_testimony';

CREATE OR REPLACE FUNCTION @extschema@.laplace_path_enrich(recorded @extschema:postgis@.geometry, incoming @extschema:postgis@.geometry) RETURNS @extschema:postgis@.geometry
    LANGUAGE c IMMUTABLE STRICT PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_path_enrich';

CREATE OR REPLACE FUNCTION @extschema@.laplace_hilbert_ranges(lo @extschema:postgis@.geometry, hi @extschema:postgis@.geometry, budget integer DEFAULT 64) RETURNS TABLE(lo bigint, hi bigint)
    LANGUAGE c IMMUTABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_hilbert_ranges';

CREATE OR REPLACE FUNCTION @extschema@.laplace_hilbert_ball_ranges_flat(@extschema:postgis@.geometry, double precision, integer) RETURNS bigint[]
    LANGUAGE c IMMUTABLE STRICT PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_hilbert_ball_ranges_flat';

CREATE OR REPLACE FUNCTION @extschema@.laplace_hilbert_ball_ranges(center @extschema:postgis@.geometry, radius double precision, budget integer DEFAULT 64) RETURNS TABLE(lo bigint, hi bigint)
    LANGUAGE c IMMUTABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_hilbert_ball_ranges';

-- Staging, pending-list maintenance, series accumulation, merge, diagnostics, and recoordinate.
CREATE OR REPLACE FUNCTION @extschema@.laplace_stage_own(load_token bigint, objects regclass[]) RETURNS void
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_stage_own';

CREATE OR REPLACE FUNCTION @extschema@.laplace_stage_retire() RETURNS TABLE(load_token bigint, relations bigint, allocated_bytes bigint)
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_stage_retire';

CREATE OR REPLACE FUNCTION @extschema@.laplace_accumulate_series(pending regclass, delta regclass) RETURNS bigint
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_accumulate_series';

CREATE OR REPLACE FUNCTION @extschema@.laplace_apply_series(pending regclass, delta regclass) RETURNS bigint
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_apply_series';

CREATE OR REPLACE FUNCTION @extschema@.laplace_attest_series(pending regclass, changed @extschema@.blake3[] DEFAULT NULL::@extschema@.blake3[]) RETURNS bigint
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_attest_series';

CREATE OR REPLACE FUNCTION @extschema@.laplace_attest_stage(pending regclass, changed regclass) RETURNS bigint
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_attest_stage';

CREATE OR REPLACE FUNCTION @extschema@.laplace_clean_pending_list(index_id regclass) RETURNS bigint
    LANGUAGE c VOLATILE STRICT
    AS 'MODULE_PATHNAME', 'laplace_clean_pending_list';

CREATE OR REPLACE FUNCTION @extschema@.laplace_merge_stage(entities regclass, paths regclass, defer_index boolean,
    OUT entity_rows bigint, OUT physicality_rows bigint, OUT updated_rows bigint, OUT pending_partitions oid[]) RETURNS record
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_merge_stage';

CREATE OR REPLACE FUNCTION @extschema@.laplace_merge_stage(entities regclass, paths regclass,
    OUT entity_rows bigint, OUT physicality_rows bigint, OUT updated_rows bigint) RETURNS record
    LANGUAGE sql BEGIN ATOMIC
      SELECT m.entity_rows, m.physicality_rows, m.updated_rows
      FROM @extschema@.laplace_merge_stage(entities, paths, false) m;
    END;

CREATE OR REPLACE FUNCTION @extschema@.laplace_merge_diagnostics(entities regclass, paths regclass,
    OUT reason text, OUT id @extschema@.blake3, OUT detail text, OUT recorded_ewkb bytea) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 8
    AS 'MODULE_PATHNAME', 'laplace_merge_diagnostics';

CREATE OR REPLACE FUNCTION @extschema@.laplace_recoordinate(apply boolean DEFAULT false,
    OUT entity_rows bigint, OUT physicality_rows bigint) RETURNS record
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_recoordinate';

CREATE OR REPLACE FUNCTION @extschema@.laplace_witness_trust(ids @extschema@.blake3[] DEFAULT NULL,
    OUT witness @extschema@.blake3, OUT lineage @extschema@.blake3, OUT trust double precision) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_witness_trust';

CREATE OR REPLACE FUNCTION @extschema@.laplace_measure_witness(witness @extschema@.blake3) RETURNS bigint
    LANGUAGE c VOLATILE STRICT
    AS 'MODULE_PATHNAME', 'laplace_measure_witness';

-- Audit, status, highway, and maintenance endpoints.
CREATE OR REPLACE FUNCTION @extschema@.laplace_audit_admitted(probe text DEFAULT 'mississippi') RETURNS jsonb
    LANGUAGE c STABLE PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_audit_admitted';

CREATE OR REPLACE FUNCTION @extschema@.laplace_estate_summary(exact boolean DEFAULT false) RETURNS jsonb
    LANGUAGE c STABLE PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_estate_summary';

CREATE OR REPLACE FUNCTION @extschema@.laplace_highway_audit(
    OUT list text, OUT cached bigint, OUT attested bigint,
    OUT direct_2w bigint, OUT direct_3w bigint, OUT direct_4w bigint,
    OUT xwalk_2w bigint, OUT xwalk_3w bigint, OUT xwalk_4w bigint, OUT invalid_lineage bigint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 8 PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_highway_audit';

CREATE OR REPLACE FUNCTION @extschema@.laplace_path_endpoints(ids @extschema@.blake3[],
    OUT i bigint, OUT entity @extschema@.blake3, OUT start_id @extschema@.blake3, OUT end_id @extschema@.blake3, OUT length bigint) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_path_endpoints';

CREATE OR REPLACE FUNCTION @extschema@.laplace_forward_suffix(ids @extschema@.blake3[], fan bigint,
    OUT i integer, OUT j integer, OUT paths bigint, OUT runs bigint, OUT next @extschema@.blake3, OUT times bigint) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_forward_suffix';

CREATE OR REPLACE FUNCTION @extschema@.laplace_roles_of(left_ids @extschema@.blake3[], right_ids @extschema@.blake3[], fan bigint DEFAULT NULL,
    OUT ordinal bigint, OUT entity @extschema@.blake3, OUT path @extschema:postgis@.geometry) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_roles_of';

CREATE OR REPLACE FUNCTION @extschema@.laplace_roles_of(left_ids @extschema@.blake3[], right_ids @extschema@.blake3[], fan bigint, kinds smallint,
    OUT ordinal bigint, OUT entity @extschema@.blake3, OUT path @extschema:postgis@.geometry) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_roles_of';

CREATE OR REPLACE FUNCTION @extschema@.laplace_fills_closure(seeds @extschema@.blake3[], seed_times bigint[] DEFAULT NULL, max_levels integer DEFAULT 64,
    OUT entity @extschema@.blake3, OUT times bigint, OUT tier smallint, OUT level integer) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 256 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_fills_closure';

CREATE OR REPLACE FUNCTION @extschema@.laplace_holder_counts(ids @extschema@.blake3[], floor smallint DEFAULT -1, kinds smallint DEFAULT 3, bits smallint[] DEFAULT NULL, endpoint smallint DEFAULT 0,
    OUT ordinal bigint, OUT id @extschema@.blake3, OUT claims bigint, OUT observations bigint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_holder_counts';

CREATE OR REPLACE FUNCTION @extschema@.laplace_holders_with_standings(parts @extschema@.blake3[], required @extschema@.blake3[] DEFAULT '{}', per_part boolean DEFAULT false,
    floor smallint DEFAULT -1, cap bigint DEFAULT -1, kinds smallint DEFAULT 3, bits smallint[] DEFAULT NULL, endpoint smallint DEFAULT 0,
    OUT ordinal bigint, OUT entity @extschema@.blake3, OUT path @extschema:postgis@.geometry, OUT tier smallint, OUT mask bit, OUT kind smallint, OUT hub boolean,
    OUT rating double precision, OUT deviation double precision, OUT volatility double precision, OUT matches bigint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_holders_with_standings';

CREATE OR REPLACE FUNCTION @extschema@.laplace_held_closure(ids @extschema@.blake3[], mode smallint DEFAULT 0, cap bigint DEFAULT -1, max_levels integer DEFAULT 32, max_nodes integer DEFAULT 400000,
    OUT node_idx integer, OUT entity @extschema@.blake3, OUT tier smallint, OUT keys_mask bigint, OUT is_file boolean, OUT is_metadata boolean, OUT is_hub boolean, OUT expanded boolean, OUT level integer,
    OUT child_idxs integer[], OUT child_saids smallint[], OUT child_runs integer[], OUT child_ordinals integer[], OUT cut boolean, OUT levels integer) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 256 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_held_closure';

CREATE OR REPLACE FUNCTION @extschema@.laplace_entity_coords(ids @extschema@.blake3[],
    OUT ordinal bigint, OUT id @extschema@.blake3, OUT tier smallint, OUT x double precision, OUT y double precision, OUT z double precision, OUT m double precision, OUT hilbert bigint) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_entity_coords';

CREATE OR REPLACE FUNCTION @extschema@.laplace_present_trunks(ids @extschema@.blake3[],
    OUT ordinal bigint, OUT id @extschema@.blake3) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_present_trunks';

CREATE OR REPLACE FUNCTION @extschema@.laplace_library_page(after_id @extschema@.blake3 DEFAULT NULL, page_limit integer DEFAULT 64,
    OUT entity @extschema@.blake3, OUT tier smallint, OUT path @extschema:postgis@.geometry, OUT mask bit) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_library_page';

CREATE OR REPLACE FUNCTION @extschema@.laplace_knn4d(query @extschema:postgis@.geometry, k integer DEFAULT 16, angular boolean DEFAULT false, min_tier smallint DEFAULT 0, max_tier smallint DEFAULT NULL, exclude_id @extschema@.blake3 DEFAULT NULL,
    OUT ordinal bigint, OUT id @extschema@.blake3, OUT tier smallint, OUT coord @extschema:postgis@.geometry, OUT distance double precision) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 16 PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_knn4d';

CREATE OR REPLACE FUNCTION @extschema@.laplace_witnesses_named(
    OUT witness @extschema@.blake3, OUT trust double precision, OUT lineage @extschema@.blake3, OUT record_id @extschema@.blake3,
    OUT name text, OUT parts bigint, OUT readings bigint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 64 PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_witnesses_named';

CREATE OR REPLACE FUNCTION @extschema@.laplace_code_symbols(files @extschema@.blake3[], include_text boolean DEFAULT true,
    OUT file @extschema@.blake3, OUT file_path text, OUT source_text text, OUT decl_ord integer, OUT decl_kind text, OUT decl_name text, OUT decl_source text) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_code_symbols';

CREATE OR REPLACE FUNCTION @extschema@.laplace_sweep_orphans(ids @extschema@.blake3[] DEFAULT NULL) RETURNS bigint
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_sweep_orphans';

CREATE OR REPLACE FUNCTION @extschema@.laplace_forget_witnesses(ids @extschema@.blake3[] DEFAULT NULL, records @extschema@.blake3[] DEFAULT NULL, invert boolean DEFAULT false, exclude_trunk @extschema@.blake3 DEFAULT NULL,
    OUT witnesses_removed bigint, OUT entities_removed bigint, OUT standings_recomputed bigint, OUT orphaned_claims bigint) RETURNS record
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_forget_witnesses';

CREATE OR REPLACE FUNCTION @extschema@.laplace_replay_verify(apply boolean DEFAULT false,
    OUT identical bigint, OUT different bigint, OUT applied boolean) RETURNS record
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_replay_verify';

CREATE OR REPLACE FUNCTION @extschema@.laplace_stage_flush_claims(pending regclass, delta regclass, changed regclass) RETURNS bigint
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_stage_flush_claims';

CREATE OR REPLACE FUNCTION @extschema@.laplace_stage_rewrite_witness(pending regclass, witness @extschema@.blake3) RETURNS bigint
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_stage_rewrite_witness';

CREATE OR REPLACE FUNCTION @extschema@.laplace_stage_restore_pruned(files @extschema@.blake3[], witnesses regclass, delta regclass, single_witness @extschema@.blake3 DEFAULT NULL) RETURNS bigint
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_stage_restore_pruned';

CREATE OR REPLACE FUNCTION @extschema@.laplace_stage_publish_attestations(pending regclass, affected regclass DEFAULT NULL, witness_ids @extschema@.blake3[] DEFAULT NULL,
    OUT admitted bigint, OUT consensus_rows bigint) RETURNS record
    LANGUAGE c VOLATILE
    AS 'MODULE_PATHNAME', 'laplace_stage_publish_attestations';

CREATE OR REPLACE FUNCTION @extschema@.laplace_present_mask(ids @extschema@.blake3[]) RETURNS boolean[]
    LANGUAGE c STABLE STRICT PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_present_mask';

CREATE OR REPLACE FUNCTION @extschema@.laplace_known_files(ids @extschema@.blake3[],
    OUT ordinal bigint, OUT id @extschema@.blake3) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_known_files';

CREATE OR REPLACE FUNCTION @extschema@.laplace_attestation_games(ids @extschema@.blake3[],
    OUT ordinal bigint, OUT games bigint) RETURNS SETOF record
    LANGUAGE c STABLE STRICT ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_attestation_games';

CREATE OR REPLACE FUNCTION @extschema@.laplace_standing_counts(ids @extschema@.blake3[],
    OUT matched bigint, OUT orphaned bigint) RETURNS record
    LANGUAGE c STABLE STRICT PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_standing_counts';

CREATE OR REPLACE FUNCTION @extschema@.laplace_attestation_count(target_witness @extschema@.blake3 DEFAULT NULL) RETURNS bigint
    LANGUAGE c STABLE PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_attestation_count';

CREATE OR REPLACE FUNCTION @extschema@.laplace_nonempty(
    OUT has_entities boolean, OUT has_standings boolean) RETURNS record
    LANGUAGE c STABLE PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_nonempty';

CREATE OR REPLACE FUNCTION @extschema@.laplace_entity_tier_sample(
    OUT tier smallint, OUT estimated bigint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 16 PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_entity_tier_sample';

CREATE OR REPLACE FUNCTION @extschema@.laplace_entity_tier_count(target_tier smallint) RETURNS bigint
    LANGUAGE c STABLE STRICT PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_entity_tier_count';

CREATE OR REPLACE FUNCTION @extschema@.laplace_replay_export(include_files boolean DEFAULT false,
    OUT claim @extschema@.blake3, OUT trunk @extschema@.blake3,
    OUT games double precision, OUT tokens double precision,
    OUT score double precision, OUT "position" integer) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 10000 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_replay_export';

CREATE OR REPLACE FUNCTION @extschema@.laplace_label_text(id bytea, null_if_hex boolean DEFAULT false) RETURNS text
    LANGUAGE c STABLE PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_label_text';

CREATE OR REPLACE FUNCTION @extschema@.laplace_label_array(ids bytea[], null_if_hex boolean DEFAULT false) RETURNS text[]
    LANGUAGE c STABLE PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_label_array';

CREATE OR REPLACE FUNCTION @extschema@.laplace_hilbert_neighbors(anchor @extschema:postgis@.geometry,
    exclude_id @extschema@.blake3 DEFAULT NULL, k integer DEFAULT 16, target_tier smallint DEFAULT 0,
    OUT id @extschema@.blake3, OUT hilbert bigint, OUT anchor_hilbert bigint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 32 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_hilbert_neighbors';

CREATE OR REPLACE FUNCTION @extschema@.laplace_constellation_sample(buckets integer DEFAULT 64,
    OUT ord integer, OUT id @extschema@.blake3,
    OUT x double precision, OUT y double precision, OUT z double precision, OUT m double precision,
    OUT radius double precision, OUT path_len integer, OUT hilbert bigint) RETURNS SETOF record
    LANGUAGE c STABLE ROWS 64 SUPPORT @extschema@.laplace_srf_support PARALLEL SAFE
    AS 'MODULE_PATHNAME', 'laplace_constellation_sample';
