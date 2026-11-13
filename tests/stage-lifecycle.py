#!/usr/bin/env python3
"""Prove Native load ownership with two real clients using the caller's identity.

The database is the deployed one. The test creates only its own stage tables,
named by its owner's transaction ID; every retirement is observed inside a
transaction that is rolled back, so no other load's abandoned tables are
dropped by this test. It drops its own tables when it ends. No canonical row is
inserted, modified or deleted.
"""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--psql', type=Path, required=True)
    parser.add_argument('--connection', required=True)
    args = parser.parse_args()
    command = [str(args.psql), '-XwAtq', '-v', 'ON_ERROR_STOP=1', args.connection]

    def query(sql):
        result = subprocess.run(command, input=sql, text=True, capture_output=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stderr.strip())
        return result.stdout.strip()

    owner = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
    token = None
    try:
        owner.stdin.write("SELECT pg_current_xact_id()::text || ':' || pg_backend_pid();\n")
        owner.stdin.flush()
        token, pid = owner.stdout.readline().strip().split(':')
        if not token.isdigit() or not pid.isdigit():
            raise RuntimeError('Invalid Native ownership fixture identity')
        entity, pending = f'stage.lp_content_{token}_0', f'stage.lp_observations_{token}'
        unrelated = f'stage.lifecycle_unclaimed_{token}'
        owner.stdin.write(f"""BEGIN;
CREATE UNLOGGED TABLE {entity}(id integer);
CREATE UNLOGGED TABLE {pending}(id integer);
INSERT INTO {pending} SELECT generate_series(1,2000);
DO $$ BEGIN PERFORM public.laplace_stage_own({token},ARRAY['{entity}'::regclass,'{pending}'::regclass]); END $$;
COMMIT;
SELECT 'ready';
""")
        owner.stdin.flush()
        if owner.stdout.readline().strip() != 'ready':
            raise RuntimeError('Native ownership fixture could not acquire its lease')
        query(f"CREATE UNLOGGED TABLE {unrelated}(id integer); INSERT INTO {unrelated} VALUES(42);")
        # Retirement of another load's abandoned tables is that load's cleanup,
        # not this test's: observe it only inside a rolled-back transaction.
        query(f"""BEGIN;
DO $$ BEGIN
  IF EXISTS(SELECT FROM public.laplace_stage_retire() r WHERE r.load_token={token})
     OR to_regclass('{entity}') IS NULL OR to_regclass('{pending}') IS NULL THEN
    RAISE EXCEPTION 'idle live load lost its private tables';
  END IF;
  BEGIN
    PERFORM public.laplace_stage_own({token},ARRAY['{unrelated}'::regclass]);
    RAISE EXCEPTION 'unrelated staging data acquired a load lease';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    PERFORM public.laplace_stage_own({token},ARRAY['public.entity'::regclass]);
    RAISE EXCEPTION 'canonical content acquired a private load lease';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
  BEGIN
    PERFORM public.laplace_stage_own({token},ARRAY['stage.entity'::regclass]);
    RAISE EXCEPTION 'shared staging template acquired a private load lease';
  EXCEPTION WHEN invalid_parameter_value THEN NULL; END;
END $$;
ROLLBACK;
""")
        owner.communicate('\\q\n', timeout=10)
        if owner.returncode:
            raise RuntimeError('Native owner did not disconnect cleanly')
        # PQfinish sends Terminate; wait on the exact lease while its backend
        # exits, rather than sleeping or assuming the socket close drained it.
        # Only this load's retirement row is asserted; the transaction is rolled
        # back, and this test drops its own tables below.
        rows = json.loads(query(f"""BEGIN;
SELECT pg_advisory_xact_lock(1279348804,{pid});
CREATE TEMP TABLE lifecycle_retired ON COMMIT DROP AS SELECT * FROM public.laplace_stage_retire();
DO $$ BEGIN
 IF to_regclass('{entity}') IS NOT NULL OR to_regclass('{pending}') IS NOT NULL
    OR (SELECT id FROM {unrelated})<>42
    OR EXISTS(SELECT FROM public.laplace_stage_retire() r WHERE r.load_token={token}) THEN
   RAISE EXCEPTION 'Native load cleanup lost isolation or idempotence';
 END IF;
END $$;
SELECT coalesce(json_agg(r),'[]'::json) FROM lifecycle_retired r WHERE r.load_token={token};
ROLLBACK;
""").splitlines()[-1])
        if len(rows) != 1 or rows[0]['load_token'] != int(token) or rows[0]['relations'] != 2 or rows[0]['allocated_bytes'] <= 0:
            raise RuntimeError(f'Native load retirement returned the wrong object set: {rows}')
        print(json.dumps(dict(live_owner_preserved=True, disconnected_owner_retired=True,
            unrelated_data_preserved=True, idempotent=True, retired=rows)))
    finally:
        if owner.poll() is None:
            owner.kill()
            owner.communicate(timeout=10)
        if token:
            # These names belong only to this test (its owner's transaction ID).
            query(f"DROP TABLE IF EXISTS stage.lp_content_{token}_0, stage.lp_observations_{token}, "
                  f"stage.lifecycle_unclaimed_{token};")


if __name__ == '__main__':
    main()
