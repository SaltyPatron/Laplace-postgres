\set ECHO none
\pset format unaligned
\pset tuples_only on
DO $$ BEGIN
  -- Remove per-call scope from both a frozen-bank primitive and an already
  -- parsed SQL adapter. Native SPI readers bind installed objects explicitly.
  ALTER FUNCTION public.laplace_mask_has_value(bit,text,text) SET search_path=public,pg_temp;
  ALTER FUNCTION public.laplace_coord(text) SET search_path=public,pg_temp;
  PERFORM public.laplace_restore_function_bindings();
  IF EXISTS (
    SELECT FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
    JOIN pg_depend d ON d.objid=p.oid AND d.classid='pg_proc'::regclass
    JOIN pg_extension e ON e.oid=d.refobjid AND d.refclassid='pg_extension'::regclass
    JOIN public.laplace_function_bindings() c ON c.symbol=p.prosrc
    WHERE e.extname='laplace' AND d.deptype='e'
      AND EXISTS(SELECT FROM unnest(p.proconfig) setting WHERE setting LIKE 'search_path=%')
  ) THEN RAISE EXCEPTION 'Native function binding contract was not restored'; END IF;
  IF EXISTS (
    SELECT FROM pg_proc p JOIN pg_language l ON l.oid=p.prolang
    JOIN pg_depend d ON d.objid=p.oid AND d.classid='pg_proc'::regclass
    JOIN pg_extension e ON e.oid=d.refobjid AND d.refclassid='pg_extension'::regclass
    WHERE e.extname='laplace' AND d.deptype='e' AND l.lanname='sql'
      AND (p.prosqlbody IS NULL OR EXISTS(SELECT FROM unnest(p.proconfig) s WHERE s LIKE 'search_path=%'))
  ) THEN RAISE EXCEPTION 'SQL adapters still bind names or set scope at each call'; END IF;
END $$;
SET search_path=pg_catalog,pg_temp;
DO $$ BEGIN
  IF public.ST_AsEWKB(public.laplace_coord('A')) IS DISTINCT FROM public.laplace_coord_ewkb('A')
     OR NOT public.laplace_mask_has_value(public.laplace_mask_slot('kind','claim'),'kind','claim') THEN
    RAISE EXCEPTION 'Native adapter depends on caller search_path';
  END IF;
END $$;
RESET search_path;
DO $$ DECLARE module text; BEGIN
 SELECT probin||'_peer' INTO module FROM pg_proc WHERE oid='public.laplace_cp_id(integer)'::regprocedure;
 EXECUTE format('CREATE FUNCTION pg_temp.peer_id(text) RETURNS public.blake3 AS %L,%L LANGUAGE C IMMUTABLE STRICT',module,'laplace_id');
 EXECUTE format('CREATE FUNCTION pg_temp.peer_separators() RETURNS public.blake3[] AS %L,%L LANGUAGE C IMMUTABLE',module,'laplace_separator_ids');
 IF pg_temp.peer_id('ab')<>public.laplace_id('ab') OR pg_temp.peer_separators()<>public.laplace_separator_ids() THEN
  RAISE EXCEPTION 'loaded module generations disagree on backend-owned perf-cache configuration';
 END IF;
END $$;
