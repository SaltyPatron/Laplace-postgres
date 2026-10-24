#!/usr/bin/env bash
# PostgreSQL 18: install the candidate into a private package directory and a
# newly created test database. Shared extension files and application DBs stay untouched.
set -euo pipefail
build=${1:?usage: extension-install.sh /path/to/cmake-build}
source_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
: "${LAPLACE_PG_DIR:?PostgreSQL installation required}"
: "${LAPLACE_CONNINFO:?declared PostgreSQL connection required}"
: "${LAPLACE_WORK:?declared scratch volume required}"
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
# Only the private test control file names the candidate module explicitly.
# SQL scripts and the candidate library retain their build/install bytes.
escaped=${module//\'/\'\'}
sed "s|^module_pathname = .*|module_pathname = '$escaped'|" "$share/extension/laplace.control" > "$stage/control"
mv "$stage/control" "$share/extension/laplace.control"
"$pg/psql" "$admin" -Xq -v ON_ERROR_STOP=1 -c "CREATE DATABASE $db"
created=1
connection=$(printf '%s\n' "$LAPLACE_CONNINFO" | sed "s/dbname=[^ ]*/dbname=$db/")
"$pg/psql" "$connection" -Xq -v ON_ERROR_STOP=1 -v writer="${db}_writer" -v candidate_share="$share" -v candidate_module="$module" -v gist4d_cases="$source_root/tests/sql/gist4d.sql" -v text_cases="$source_root/tests/sql/text_id.sql" -v projection_cases="$source_root/tests/sql/projection.sql" <<'SQL'
SELECT set_config('extension_control_path', :'candidate_share' || ':$system', false);
CREATE EXTENSION postgis;
CREATE EXTENSION laplace;
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
GRANT INSERT,TRUNCATE ON public.physicality_00 TO :"writer";
GRANT EXECUTE ON FUNCTION public.laplace_clean_pending_list(regclass) TO :"writer";
CREATE TABLE public.candidate_unrelated(values int[]);
CREATE INDEX candidate_unrelated_gin ON public.candidate_unrelated USING gin(values);
GRANT INSERT,TRUNCATE ON public.candidate_unrelated TO :"writer";
SET SESSION AUTHORIZATION :"writer";
DO $$ DECLARE selected regclass; BEGIN
  SELECT i.indexrelid INTO STRICT selected FROM pg_index i
    JOIN pg_class c ON c.oid=i.indexrelid JOIN pg_am a ON a.oid=c.relam
    WHERE i.indrelid='public.physicality_00'::regclass AND a.amname='gin';
  BEGIN
    PERFORM pg_catalog.gin_clean_pending_list(selected);
    RAISE EXCEPTION 'runtime unexpectedly owns the index';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
  PERFORM public.laplace_clean_pending_list(selected);
  SELECT i.indexrelid INTO STRICT selected FROM pg_index i
    JOIN pg_class c ON c.oid=i.indexrelid JOIN pg_am a ON a.oid=c.relam
    WHERE i.indrelid='public.physicality_01'::regclass AND a.amname='gin';
  BEGIN
    PERFORM public.laplace_clean_pending_list(selected);
    RAISE EXCEPTION 'unwritable Laplace index maintenance was permitted';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
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
\i :projection_cases
SQL
printf 'PASS candidate extension installed and executed in isolated database %s\n' "$db"
