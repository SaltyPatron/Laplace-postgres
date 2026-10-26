\echo Use "ALTER EXTENSION laplace UPDATE TO '1.38'" to load this file. \quit

CREATE FUNCTION laplace_function_bindings(OUT symbol text,OUT sql_scope boolean)
RETURNS SETOF record AS 'MODULE_PATHNAME','laplace_function_bindings'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION laplace_restore_function_bindings() RETURNS void
LANGUAGE plpgsql VOLATILE SET search_path=pg_catalog,public,pg_temp AS $$
DECLARE binding record; definition text; body text; body_at integer;
BEGIN
  -- The module's exported contract covers ROM intrinsics and SQL/SPI owners.
  -- Consumers must not infer this from volatility or maintain exception lists.
  FOR binding IN
    SELECT p.oid,format('%I.%I(%s)',n.nspname,p.proname,pg_get_function_identity_arguments(p.oid)) AS signature,
           c.sql_scope,p.prosecdef
    FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
    JOIN pg_language l ON l.oid=p.prolang
    JOIN pg_depend d ON d.objid=p.oid AND d.classid='pg_proc'::regclass
    JOIN pg_extension e ON e.oid=d.refobjid AND d.refclassid='pg_extension'::regclass
    LEFT JOIN public.laplace_function_bindings() c ON c.symbol=p.prosrc
    WHERE e.extname='laplace' AND d.deptype='e' AND p.prokind='f' AND l.lanname='c'
      AND p.probin=(SELECT probin FROM pg_proc WHERE oid='public.laplace_function_bindings()'::regprocedure)
  LOOP
    IF binding.sql_scope IS NULL THEN
      RAISE EXCEPTION 'Native function has no binding contract: %',binding.signature;
    END IF;
    IF binding.sql_scope OR binding.prosecdef THEN
      EXECUTE 'ALTER FUNCTION '||binding.signature||' SET search_path=public,pg_temp';
    ELSE
      EXECUTE 'ALTER FUNCTION '||binding.signature||' RESET search_path';
    END IF;
  END LOOP;

  -- Bind thin SQL adapters once at definition time. Parsed bodies retain
  -- their referenced OIDs and need no per-call search_path setting.
  FOR binding IN
    SELECT p.oid,format('%I.%I(%s)',n.nspname,p.proname,pg_get_function_identity_arguments(p.oid)) AS signature,
           p.prosrc,p.prosqlbody IS NOT NULL AS parsed,p.prosecdef
    FROM pg_proc p JOIN pg_namespace n ON n.oid=p.pronamespace
    JOIN pg_language l ON l.oid=p.prolang
    JOIN pg_depend d ON d.objid=p.oid AND d.classid='pg_proc'::regclass
    JOIN pg_extension e ON e.oid=d.refobjid AND d.refclassid='pg_extension'::regclass
    WHERE e.extname='laplace' AND d.deptype='e' AND p.prokind='f' AND l.lanname='sql'
  LOOP
    IF binding.prosecdef THEN CONTINUE; END IF;
    EXECUTE 'ALTER FUNCTION '||binding.signature||' RESET search_path';
    IF NOT binding.parsed THEN
      definition:=pg_get_functiondef(binding.oid);
      body_at:=strpos(definition,E'\nAS ');
      IF body_at=0 THEN RAISE EXCEPTION 'Cannot bind SQL adapter %',binding.signature; END IF;
      body:=rtrim(binding.prosrc);
      IF right(body,1)<>';' THEN body:=body||E'\n;'; END IF;
      EXECUTE left(definition,body_at)||E'BEGIN ATOMIC\n'||body||E'\nEND';
    END IF;
  END LOOP;
END $$;
SELECT laplace_restore_function_bindings();
