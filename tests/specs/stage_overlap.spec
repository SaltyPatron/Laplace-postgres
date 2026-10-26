setup
{
  SELECT laplace_ensure_stage();
  CREATE UNLOGGED TABLE stage.overlap_a_entities (LIKE stage.entity INCLUDING DEFAULTS);
  CREATE UNLOGGED TABLE stage.overlap_a_paths (LIKE stage.physicality INCLUDING DEFAULTS);
  CREATE UNLOGGED TABLE stage.overlap_b_entities (LIKE stage.entity INCLUDING DEFAULTS);
  CREATE UNLOGGED TABLE stage.overlap_b_paths (LIKE stage.physicality INCLUDING DEFAULTS);
  INSERT INTO stage.overlap_a_entities(id,tier,coord,hilbert)
    SELECT laplace_id('overlap'),laplace_tier('overlap'),laplace_coord('overlap'),laplace_hilbert('overlap');
  INSERT INTO stage.overlap_a_paths(entity,tier,hilbert,path)
    SELECT laplace_id('overlap'),laplace_tier('overlap'),laplace_hilbert('overlap'),
      laplace_path(ARRAY(SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table('overlap','') c));
  INSERT INTO stage.overlap_b_entities SELECT * FROM stage.overlap_a_entities;
  INSERT INTO stage.overlap_b_paths SELECT * FROM stage.overlap_a_paths;
}
teardown
{
  DELETE FROM public.physicality WHERE entity=laplace_id('overlap');
  DELETE FROM public.entity WHERE id=laplace_id('overlap');
  DROP TABLE stage.overlap_a_entities,stage.overlap_a_paths,stage.overlap_b_entities,stage.overlap_b_paths;
}
session "first"
step "begin_first" { BEGIN; }
step "publish_first" { SELECT * FROM laplace_merge_stage('stage.overlap_a_entities','stage.overlap_a_paths'); }
step "commit_first" { COMMIT; }
session "second"
setup { SET application_name='laplace_stage_overlap'; }
step "publish_second" { SELECT * FROM laplace_merge_stage('stage.overlap_b_entities','stage.overlap_b_paths'); }
session "observer"
step "overlap_wait" {
  SELECT wait_event_type,wait_event FROM pg_stat_activity
  WHERE datname=current_database() AND application_name='laplace_stage_overlap';
}
step "verify_complete" {
  SELECT (SELECT count(*) FROM public.entity WHERE id=laplace_id('overlap')) AS entities,
         (SELECT count(*) FROM public.physicality WHERE entity=laplace_id('overlap')) AS paths,
         (SELECT count(*) FROM stage.overlap_b_entities) AS pending_entities,
         (SELECT count(*) FROM stage.overlap_b_paths) AS pending_paths;
}
permutation "begin_first" "publish_first" "publish_second" "overlap_wait" "commit_first" "verify_complete"
