-- Querying models' testimony (experimental). IDs are computed in place; everything else is an index lookup.
\timing on
SET enable_parallel_append = off;
-- A token written with a leading space is the composition [' ', word].
CREATE OR REPLACE FUNCTION pg_temp.sp(text) RETURNS uuid LANGUAGE sql IMMUTABLE
  AS $$ SELECT laplace_compose(ARRAY[laplace_cp_id(32), laplace_text_id($1)]) $$;

\echo '== 1. What do the models say is near " king"? Every claim, its standing, and which families attest it'
SELECT laplace_text_id(' ') AS space \gset
SELECT o.id AS object, round(s.rating) AS rating, round(s.deviation) AS rd,
       (SELECT string_agg(w.name, ', ' ORDER BY w.name) FROM witness w WHERE w.id = ANY (s.witnesses)) AS witnesses
FROM claim c JOIN standing s ON s.claim = c.id JOIN entity o ON o.id = c.object
WHERE c.subject = pg_temp.sp('king') AND c.predicate = laplace_text_id('near')
ORDER BY cardinality(s.witnesses) DESC, s.rating DESC LIMIT 12;

\echo '== 2. Relations about " king" that two or more independent families agree on'
SELECT count(*) AS agreed FROM claim c JOIN standing s ON s.claim = c.id
WHERE c.subject = pg_temp.sp('king') AND c.predicate = laplace_text_id('near')
  AND (SELECT count(DISTINCT w.lineage) FROM witness w WHERE w.id = ANY (s.witnesses)) >= 2;

\echo '== 3. Every model claim about " dog" that lands on a word the curated web lexicalizes with the same concept'
SELECT count(*) AS same_concept FROM claim c
JOIN physicality p ON p.entity = c.object AND p.tier = 3
JOIN claim l1 ON l1.subject = laplace_text_id('dog') AND l1.predicate = laplace_text_id('eng')
JOIN claim l2 ON l2.object = l1.object AND l2.predicate = laplace_text_id('eng')
     AND l2.subject = ANY (laplace_vertex_ids(p.path)) AND l2.subject <> laplace_text_id('dog')
WHERE c.subject = pg_temp.sp('dog') AND c.predicate = laplace_text_id('near');

\echo '== 4. The ledger for one claim: who said " king" is near " queen", with what z'
SELECT w.name, a.z, a.seq FROM attestation a JOIN witness w ON w.id = a.witness
WHERE a.claim = (SELECT id FROM claim WHERE subject = pg_temp.sp('king') AND predicate = laplace_text_id('near') AND object = pg_temp.sp('queen'))
ORDER BY a.seq;

\echo '== 5. Model testimony per witness: claims, and how many other families corroborate'
SELECT w.name, count(*) AS attestations,
       count(*) FILTER (WHERE cardinality(s.witnesses) >= 2) AS corroborated
FROM attestation a JOIN witness w ON w.id = a.witness AND w.kind = 'model' JOIN standing s ON s.claim = a.claim
GROUP BY w.name ORDER BY 2 DESC;
