#!/usr/bin/env bash
# Database checks of the deployed extension, run by Laplace-Operations' `tests`
# stage after deployment against the deployed database. The checked-out
# revision is the deployed one. Nothing is created or dropped except this run's
# own uniquely named stage tables and scratch roles: every regression case runs
# in a transaction it rolls back, pg_regress runs with --use-existing (no
# database, role or server is created), and no extension object is replaced.
#
# Environment:
#   LAPLACE_CONNINFO  libpq key=value connection naming the deployed database
#   LAPLACE_PG_DIR    PostgreSQL installation (bin/psql, lib/pgxs test drivers)
#   PG_CONFIG         optional; locates the test drivers (default $LAPLACE_PG_DIR/bin/pg_config)
#   TMPDIR            scratch directory for results and diffs
set -euo pipefail
: "${LAPLACE_CONNINFO:?deployed database connection required}"
: "${LAPLACE_PG_DIR:?PostgreSQL installation required}"
: "${TMPDIR:?scratch directory required}"
source_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
pg_config=${PG_CONFIG:-$LAPLACE_PG_DIR/bin/pg_config}
pgxs=$(dirname "$("$pg_config" --pgxs)")/..
bindir=$("$pg_config" --bindir)
psql="$bindir/psql"
pg_regress="$pgxs/test/regress/pg_regress"
isolationtester="$pgxs/test/isolation/isolationtester"
for tool in "$psql" "$pg_regress" "$isolationtester"; do
  [[ -x $tool ]] || { echo "deployed.sh: missing PostgreSQL test driver $tool" >&2; exit 2; }
done

# pg_regress takes the database, host, port and user as options; every other
# libpq keyword reaches psql through its environment variable.
dbname= host= port= user=
set -f
for pair in $LAPLACE_CONNINFO; do
  [[ $pair == *=* && $pair != *\'* && $pair != *\"* && $pair != *,* ]] ||
    { echo "deployed.sh: LAPLACE_CONNINFO must be unquoted single-host key=value pairs: $pair" >&2; exit 2; }
  key=${pair%%=*} value=${pair#*=}
  case $key in
    dbname) dbname=$value ;;
    host) host=$value ;;
    port) port=$value ;;
    user) user=$value ;;
    hostaddr) export PGHOSTADDR=$value ;;
    sslmode) export PGSSLMODE=$value ;;
    sslrootcert) export PGSSLROOTCERT=$value ;;
    sslcert) export PGSSLCERT=$value ;;
    sslkey) export PGSSLKEY=$value ;;
    passfile) export PGPASSFILE=$value ;;
    connect_timeout) export PGCONNECT_TIMEOUT=$value ;;
    require_auth) export PGREQUIREAUTH=$value ;;
    application_name) ;;
    *) echo "deployed.sh: unsupported LAPLACE_CONNINFO keyword $key" >&2; exit 2 ;;
  esac
done
set +f
[[ -n $dbname ]] || { echo 'deployed.sh: LAPLACE_CONNINFO names no database' >&2; exit 2; }
regress_connection=(--dbname="$dbname")
[[ -z $host ]] || regress_connection+=(--host="$host")
[[ -z $port ]] || regress_connection+=(--port="$port")
[[ -z $user ]] || regress_connection+=(--user="$user")
sql() { "$psql" "$LAPLACE_CONNINFO" -XAtq -v ON_ERROR_STOP=1 "$@"; }

work=$(mktemp -d "$TMPDIR/laplace-postgres-deployed.XXXXXX")
trap 'rm -rf "$work"' EXIT

# The deployed extension: installed in public, every C function bound to the
# installed module.
[[ $(sql -c "SELECT extnamespace::regnamespace FROM pg_extension WHERE extname='laplace'") == public ]] ||
  { echo 'deployed.sh: extension laplace is not installed in schema public' >&2; exit 1; }
sql <<'SQL'
DO $$ BEGIN
  -- The build names the module by its content (cmake/PackageModule.cmake: $libdir/laplace_<20 hex>).
  IF NOT (SELECT count(*) > 0 AND count(DISTINCT p.probin) = 1
                 AND bool_and(p.probin ~ '^\$libdir/laplace(_[0-9a-f]{20})?$')
          FROM pg_proc p JOIN pg_depend d ON d.classid='pg_proc'::regclass AND d.objid=p.oid
          JOIN pg_extension e ON e.oid=d.refobjid AND d.refclassid='pg_extension'::regclass
          WHERE e.extname='laplace' AND d.deptype='e' AND p.probin IS NOT NULL) THEN
    RAISE EXCEPTION 'extension functions are not all bound to one installed laplace module: %',
      (SELECT string_agg(DISTINCT p.probin, ', ')
       FROM pg_proc p JOIN pg_depend d ON d.classid='pg_proc'::regclass AND d.objid=p.oid
       JOIN pg_extension e ON e.oid=d.refobjid AND d.refclassid='pg_extension'::regclass
       WHERE e.extname='laplace' AND d.deptype='e' AND p.probin IS NOT NULL);
  END IF;
END $$;
SQL
echo 'PASS extension functions are bound to one installed laplace module'

# Regression cases. Results go to scratch; psql's relative include of the
# shared geometry fixture resolves against the driver's working directory.
cp "$source_root/tests/sql/geometry_cases.sql" "$work/"
mapfile -t cases < <(tr -d '\r' < "$source_root/tests/regression_cases.txt" | grep -v '^[[:space:]]*$')
if ! (cd "$work" && "$pg_regress" --use-existing "${regress_connection[@]}" --bindir="$bindir" \
      --inputdir="$source_root/tests" --outputdir="$work/regress" "${cases[@]}"); then
  [[ ! -f $work/regress/regression.diffs ]] || cat "$work/regress/regression.diffs"
  exit 1
fi

# Ingestion writer privileges: a scratch role inside a rolled-back transaction.
# The writer may clean Laplace GIN pending lists of partitions it can write and
# nothing else, and has no canonical-table TRUNCATE.
sql -v writer="laplace_check_writer_$$" <<'SQL'
BEGIN;
CREATE ROLE :"writer";
GRANT USAGE ON SCHEMA public TO :"writer";
GRANT INSERT,UPDATE ON public.physicality_00 TO :"writer";
CREATE UNLOGGED TABLE stage.check_writer_maintenance_keys (entity blake3);
CREATE UNLOGGED TABLE stage.check_writer_unwritable_keys (entity blake3);
INSERT INTO stage.check_writer_maintenance_keys VALUES ('00000000000000000000000000000000');
INSERT INTO stage.check_writer_unwritable_keys VALUES ('01000000000000000000000000000000');
GRANT USAGE ON SCHEMA stage TO :"writer";
GRANT SELECT ON stage.check_writer_maintenance_keys,stage.check_writer_unwritable_keys TO :"writer";
CREATE TABLE public.check_writer_unrelated(values int[]);
CREATE INDEX check_writer_unrelated_gin ON public.check_writer_unrelated USING gin(values);
GRANT INSERT,TRUNCATE ON public.check_writer_unrelated TO :"writer";
SET LOCAL SESSION AUTHORIZATION :"writer";
DO $$ DECLARE selected regclass; BEGIN
  IF has_table_privilege('public.physicality_00','TRUNCATE') THEN
    RAISE EXCEPTION 'ingestion unexpectedly has canonical-table TRUNCATE';
  END IF;
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
  PERFORM public.laplace_clean_stage('stage.check_writer_maintenance_keys');
  BEGIN
    PERFORM public.laplace_clean_stage('stage.check_writer_unwritable_keys');
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
    PERFORM public.laplace_clean_pending_list('public.check_writer_unrelated_gin'::regclass);
    RAISE EXCEPTION 'unrelated index maintenance was permitted';
  EXCEPTION WHEN insufficient_privilege THEN NULL; END;
END $$;
ROLLBACK;
SQL
echo 'PASS ingestion writer privileges'

# Load ownership with two real clients; only this run's own stage tables.
python3 "$source_root/tests/stage-lifecycle.py" --psql "$psql" --connection "$LAPLACE_CONNINFO"

# Concurrent publication of the same content; both publications roll back.
"$isolationtester" "$LAPLACE_CONNINFO" < "$source_root/tests/specs/stage_overlap.spec" > "$work/stage-overlap.out"
sed -i -e 's/[[:blank:]]*$//' -e '${/^$/d;}' "$work/stage-overlap.out"
tr -d '\r' < "$source_root/tests/expected/stage_overlap.out" > "$work/stage-overlap.expected"
diff -u "$work/stage-overlap.expected" "$work/stage-overlap.out"
echo "PASS deployed extension regressions against database $dbname"
