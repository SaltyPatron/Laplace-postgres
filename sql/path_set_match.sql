-- Register before cold-install index creation initializes PostgreSQL's cached
-- operator-class support list. The same definition serves in-place updates.
CREATE OR REPLACE FUNCTION @extschema@.laplace_gin_compare_partial(bytea,bytea,smallint,internal) RETURNS integer
 AS 'MODULE_PATHNAME','laplace_gin_compare_partial' LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;
DO $path_set_match$
BEGIN
 IF NOT EXISTS(SELECT FROM pg_catalog.pg_amproc p JOIN pg_catalog.pg_opfamily f ON f.oid=p.amprocfamily
   JOIN pg_catalog.pg_am a ON a.oid=f.opfmethod
   WHERE f.opfnamespace=(SELECT extnamespace FROM pg_catalog.pg_extension WHERE extname='laplace')
     AND f.opfname='laplace_path_ops' AND a.amname='gin' AND p.amprocnum=5) THEN
  ALTER OPERATOR FAMILY @extschema@.laplace_path_ops USING gin ADD
   FUNCTION 5 (@extschema:postgis@.geometry,@extschema:postgis@.geometry)
    @extschema@.laplace_gin_compare_partial(bytea,bytea,smallint,internal);
 END IF;
END $path_set_match$;

