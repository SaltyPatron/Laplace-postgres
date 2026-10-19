CREATE TEMP TABLE geometry_cases (name text PRIMARY KEY, path geometry, expected blake3[]);
INSERT INTO geometry_cases
SELECT 'strand-' || n, laplace_path(ids), ids
FROM (SELECT n, array_agg(laplace_cp_id(96 + i) ORDER BY i) AS ids
      FROM unnest(ARRAY[2,3,8]) n CROSS JOIN LATERAL generate_series(1,n) i GROUP BY n) s;
INSERT INTO geometry_cases VALUES
('point', laplace_path(ARRAY[laplace_cp_id(97)]), ARRAY[laplace_cp_id(97)]),
('runs', laplace_path(ARRAY[laplace_cp_id(97),laplace_cp_id(97),laplace_cp_id(98),laplace_cp_id(97)]),
 ARRAY[laplace_cp_id(97),laplace_cp_id(98)]);
INSERT INTO geometry_cases
SELECT 'polygon', ST_MakePolygon(laplace_path(ARRAY[laplace_cp_id(97),laplace_cp_id(98),laplace_cp_id(99),laplace_cp_id(97)])),
       ARRAY[laplace_cp_id(97),laplace_cp_id(98),laplace_cp_id(99)];
INSERT INTO geometry_cases
SELECT 'multipolygon', ST_Multi(path), expected FROM geometry_cases WHERE name = 'polygon';
INSERT INTO geometry_cases
SELECT 'multiline', ST_Multi(path), expected FROM geometry_cases WHERE name = 'strand-8';
INSERT INTO geometry_cases
SELECT 'multipoint', ST_Multi(path), expected FROM geometry_cases WHERE name = 'point';
INSERT INTO geometry_cases
SELECT 'collection', ST_Collect(array_agg(path)),
       ARRAY[laplace_cp_id(97),laplace_cp_id(98),laplace_cp_id(99),laplace_cp_id(100),laplace_cp_id(101),laplace_cp_id(102),laplace_cp_id(103),laplace_cp_id(104)]
FROM geometry_cases WHERE name IN ('multipolygon', 'strand-8');
INSERT INTO geometry_cases
SELECT 'empty-' || kind, ST_GeomFromText(kind || ' ZM EMPTY'), '{}'::blake3[]
FROM unnest(ARRAY['POINT','LINESTRING','POLYGON','MULTIPOINT','MULTILINESTRING','MULTIPOLYGON','GEOMETRYCOLLECTION']) kind;
