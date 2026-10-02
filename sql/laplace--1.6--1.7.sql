-- COUPLE as one coarse native operator (Sequence 20.2; spec 36, native execution grain): the strands, the containment
-- and the shape of an admitted observation in one call, set access through SPI and everything else native. shape:
-- -1 none, 0 Fréchet, 1 Fréchet with shape_n outliers, 2 DTW, 3 EDR within shape_n; keep: how many nearest curves.
CREATE FUNCTION laplace_couple(occ blake3[], fan bigint, refuse blake3[], shape smallint, shape_n double precision, keep integer,
  OUT entity blake3, OUT occ integer, OUT route smallint, OUT rating double precision, OUT deviation double precision, OUT volatility double precision,
  OUT via blake3, OUT rel blake3, OUT tier smallint, OUT distance double precision, OUT vertices blake3[]) RETURNS SETOF record
  AS 'MODULE_PATHNAME', 'laplace_couple' LANGUAGE C STABLE STRICT PARALLEL SAFE ROWS 4096;
