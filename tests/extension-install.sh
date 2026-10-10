#!/usr/bin/env bash
# PostgreSQL 18: install the candidate into a private package directory and a
# newly created test database. Shared extension files and application DBs stay untouched.
set -euo pipefail
build=${1:?usage: extension-install.sh /path/to/cmake-build}
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
"$pg/psql" "$connection" -Xq -v ON_ERROR_STOP=1 -v candidate_share="$share" -v candidate_module="$module" <<'SQL'
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
SQL
printf 'PASS candidate extension installed and executed in isolated database %s\n' "$db"
