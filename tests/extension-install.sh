#!/usr/bin/env bash
# PostgreSQL 18: install the candidate into a private package directory and a
# newly created test database. Shared extension files and application DBs stay untouched.
set -euo pipefail
build=${1:?usage: extension-install.sh /path/to/cmake-build}
node "$(dirname "$0")/../tools/check-function-bindings.mjs" "$build"
source_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
: "${LAPLACE_PG_DIR:?PostgreSQL installation required}"
: "${LAPLACE_CONNINFO:?declared PostgreSQL connection required}"
: "${LAPLACE_WORK:?declared scratch volume required}"
# Candidate connections select the same private cache closure as the candidate
# reader. No cluster or application database settings are changed.
: "${LAPLACE_TIER0:?candidate Tier-0 cache required}" "${LAPLACE_HIGHWAY:?candidate Highway required}"
LAPLACE_FLAGS=${LAPLACE_FLAGS:-${LAPLACE_TIER0%.bin}.flags}
export PGOPTIONS="${PGOPTIONS:+$PGOPTIONS }-c laplace.tier0=$LAPLACE_TIER0 -c laplace.flags=$LAPLACE_FLAGS -c laplace.highway=$LAPLACE_HIGHWAY"
pg="$LAPLACE_PG_DIR/bin"
admin=$(printf '%s\n' "$LAPLACE_CONNINFO" | sed 's/dbname=[^ ]*/dbname=postgres/')
[[ $admin == *'dbname=postgres'* ]] || { echo 'connection must name a database'; exit 2; }
stage=$(mktemp -d "$LAPLACE_WORK/extension-ci.XXXXXX")
chmod 0755 "$stage" # PostgreSQL must traverse this nonsecret candidate package.
db=laplace_ci_$(date -u +%Y%m%d%H%M%S)_$$
created=0
cleanup() {
  local result=$?
  trap - EXIT
  if [[ $created == 1 ]] && ! "$pg/psql" "$admin" -Xq -v ON_ERROR_STOP=1 -c "DROP DATABASE $db"; then
    echo "test database cleanup failed; candidate package retained at $stage" >&2
    exit 1
  fi
  "$pg/psql" "$admin" -Xq -v ON_ERROR_STOP=1 -c "DROP ROLE IF EXISTS ${db}_writer"
  rm -rf "$stage"
  exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
DESTDIR="$stage" cmake --install "$build" >/dev/null
share="$stage$("$pg/pg_config" --sharedir)"
module="$stage$("$pg/pg_config" --pkglibdir)/laplace"
[[ -s "$share/extension/laplace.control" && -s "$module.so" ]]
cp "$module.so" "${module}_peer.so"
# Only the private test control file names the candidate module explicitly.
# SQL scripts and the candidate library retain their build/install bytes.
escaped=${module//\'/\'\'}
sed "s|^module_pathname = .*|module_pathname = '$escaped'|" "$share/extension/laplace.control" > "$stage/control"
mv "$stage/control" "$share/extension/laplace.control"
"$pg/psql" "$admin" -Xq -v ON_ERROR_STOP=1 -c "CREATE DATABASE $db"
created=1
connection=$(printf '%s\n' "$LAPLACE_CONNINFO" | sed "s/dbname=[^ ]*/dbname=$db/")
"$pg/psql" "$connection" -Xq -v ON_ERROR_STOP=1 -v claim_cases="$source_root/tests/sql/claims.sql" -v holder_cases="$source_root/tests/sql/holders.sql" -v stored_cases="$source_root/tests/sql/stored_rows.sql" -v schema_cases="$source_root/tests/sql/schema.sql" -v writer="${db}_writer" -v candidate_share="$share" -v candidate_module="$module" -v gist4d_cases="$source_root/tests/sql/gist4d.sql" -v text_cases="$source_root/tests/sql/text_id.sql" -v projection_cases="$source_root/tests/sql/projection.sql" -v attestation_cases="$source_root/tests/sql/attestation.sql" -v stage_cases="$source_root/tests/sql/stage.sql" -v routing_cases="$source_root/tests/sql/stage_routing.sql" -v annotation_cases="$source_root/tests/sql/path_annotations.sql" -v gin_cases="$source_root/tests/sql/gin_path.sql" -v geometry_suite="$source_root/tests/sql/geometry.sql" -v frontier_cases="$source_root/tests/sql/frontier_runtime.sql" -v depth_cases="$source_root/tests/sql/realization_depth.sql" -v container_cases="$source_root/tests/sql/container_frontier.sql" <<'SQL'
SELECT set_config('extension_control_path', :'candidate_share' || ':$system', false);
CREATE EXTENSION postgis;
-- Install the current schema directly; unreleased development history is not
-- a supported installation path. gist4d.sql also checks a shared namespace.
CREATE EXTENSION laplace;
\i :stored_cases
\i :holder_cases
\i :claim_cases
\i :schema_cases
SELECT extname, extversion FROM pg_extension WHERE extname='laplace';
SELECT laplace_isa();
CREATE TEMP TABLE candidate_module_check AS
SELECT count(*) > 0 AND bool_and(p.probin = :'candidate_module') AS correct
FROM pg_proc p JOIN pg_depend d ON d.classid='pg_proc'::regclass AND d.objid=p.oid
JOIN pg_extension e ON e.oid=d.refobjid AND d.refclassid='pg_extension'::regclass
WHERE e.extname='laplace' AND p.probin IS NOT NULL;
DO $$ BEGIN
  IF NOT (SELECT correct FROM candidate_module_check) THEN
    RAISE EXCEPTION 'extension functions are not bound to the candidate module';
  END IF;
END $$;
CREATE ROLE :"writer";
GRANT USAGE ON SCHEMA public TO :"writer";
GRANT INSERT,UPDATE ON public.physicality_00 TO :"writer";
SELECT public.laplace_ensure_stage();
CREATE UNLOGGED TABLE stage.candidate_maintenance_keys (entity blake3);
CREATE UNLOGGED TABLE stage.candidate_unwritable_keys (entity blake3);
INSERT INTO stage.candidate_maintenance_keys VALUES ('00000000000000000000000000000000');
INSERT INTO stage.candidate_unwritable_keys VALUES ('01000000000000000000000000000000');
GRANT USAGE ON SCHEMA stage TO :"writer";
GRANT SELECT ON stage.candidate_maintenance_keys,stage.candidate_unwritable_keys TO :"writer";
CREATE TABLE public.candidate_unrelated(values int[]);
CREATE INDEX candidate_unrelated_gin ON public.candidate_unrelated USING gin(values);
GRANT INSERT,TRUNCATE ON public.candidate_unrelated TO :"writer";
SET SESSION AUTHORIZATION :"writer";
DO $$ DECLARE selected regclass; BEGIN
  BEGIN
    TRUNCATE public.physicality_00;
    RAISE EXCEPTION 'ingestion unexpectedly has canonical-table TRUNCATE';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  FOR selected IN SELECT i.indexrelid FROM pg_index i
    JOIN pg_class c ON c.oid=i.indexrelid JOIN pg_am a ON a.oid=c.relam
    WHERE i.indrelid='public.physicality_00'::regclass AND a.amname='gin' LOOP
  BEGIN
    PERFORM pg_catalog.gin_clean_pending_list(selected);
    RAISE EXCEPTION 'runtime unexpectedly owns the index';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  PERFORM public.laplace_clean_pending_list(selected);
  END LOOP;
  IF NOT FOUND THEN RAISE EXCEPTION 'canonical GIN maintenance fixture is missing'; END IF;
  PERFORM public.laplace_clean_stage('stage.candidate_maintenance_keys');
  BEGIN
    PERFORM public.laplace_clean_stage('stage.candidate_unwritable_keys');
    RAISE EXCEPTION 'stage routing bypassed index maintenance privileges';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  FOR selected IN SELECT i.indexrelid FROM pg_index i
    JOIN pg_class c ON c.oid=i.indexrelid JOIN pg_am a ON a.oid=c.relam
    WHERE i.indrelid='public.physicality_01'::regclass AND a.amname='gin' LOOP
  BEGIN
    PERFORM public.laplace_clean_pending_list(selected);
    RAISE EXCEPTION 'unwritable Laplace index maintenance was permitted';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  END LOOP;
  BEGIN
    PERFORM public.laplace_clean_pending_list('public.candidate_unrelated_gin'::regclass);
    RAISE EXCEPTION 'unrelated index maintenance was permitted';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
END $$;
RESET SESSION AUTHORIZATION;
-- Run the real KNN and shared-namespace cold-install cases with this session's
-- candidate package path, rather than accidentally loading an installed copy.
\i :gist4d_cases
\i :text_cases
\i :depth_cases
\i :projection_cases
\i :attestation_cases
\i :stage_cases
\i :routing_cases
\i :annotation_cases
\i :gin_cases
DROP TABLE geometry_cases;
\i :geometry_suite
\i :frontier_cases
\i :container_cases
SQL
"$pg/psql" "$connection" -Xq -v ON_ERROR_STOP=1 -f "$source_root/tests/sql/function_bindings.sql"
python3 "$source_root/tests/stage-lifecycle.py" --psql "$pg/psql" --connection "$connection"
isolation="$LAPLACE_PG_DIR/lib/pgxs/src/test/isolation/isolationtester"
[[ -x "$isolation" ]] || { echo "PostgreSQL isolation tester required: $isolation" >&2; exit 1; }
"$isolation" "$connection" < "$source_root/tests/specs/stage_overlap.spec" > "$stage/stage-overlap.out"
sed -i -e 's/[[:blank:]]*$//' -e '${/^$/d;}' "$stage/stage-overlap.out"
cat "$stage/stage-overlap.out"
diff -u "$source_root/tests/expected/stage_overlap.out" "$stage/stage-overlap.out"
printf 'PASS candidate extension installed and executed in isolated database %s\n' "$db"
