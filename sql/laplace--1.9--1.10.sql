-- 1.10 keys entities, physicalities and attestations by their IDs and partitions entities and physicalities by the ID
-- hash alone. The tables of 1.9 are partitioned by tier: no ALTER turns one into the other, and the rows they hold
-- include the same content stored more than once. A 1.9 database is dropped and deployed again (deploy.sh drop).
DO $$ BEGIN RAISE EXCEPTION 'laplace 1.10 changes how entities and physicalities are keyed and partitioned: drop and deploy the database again (the CI deploy with drop)'; END $$;
