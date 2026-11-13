# Two loads publish the same content while the first is still in flight. The
# second waits on the first's insertion and then completes exactly once. Both
# publications are rolled back: no canonical row outlives the test. The setup
# and teardown create and drop only this test's own stage tables.
setup
{
  DROP TABLE IF EXISTS stage.check_overlap_a_entities,stage.check_overlap_a_paths,
    stage.check_overlap_b_entities,stage.check_overlap_b_paths;
  CREATE UNLOGGED TABLE stage.check_overlap_a_entities (LIKE stage.entity INCLUDING DEFAULTS);
  CREATE UNLOGGED TABLE stage.check_overlap_a_paths (LIKE stage.physicality INCLUDING DEFAULTS);
  CREATE UNLOGGED TABLE stage.check_overlap_b_entities (LIKE stage.entity INCLUDING DEFAULTS);
  CREATE UNLOGGED TABLE stage.check_overlap_b_paths (LIKE stage.physicality INCLUDING DEFAULTS);
  INSERT INTO stage.check_overlap_a_entities(id,tier,coord,hilbert)
    SELECT laplace_id('lpcheckstageoverlap'),laplace_tier('lpcheckstageoverlap'),
      laplace_coord('lpcheckstageoverlap'),laplace_hilbert('lpcheckstageoverlap');
  INSERT INTO stage.check_overlap_a_paths(entity,tier,hilbert,path)
    SELECT laplace_id('lpcheckstageoverlap'),laplace_tier('lpcheckstageoverlap'),laplace_hilbert('lpcheckstageoverlap'),
      laplace_path(ARRAY(SELECT laplace_cp_id(ascii(c)) FROM regexp_split_to_table('lpcheckstageoverlap','') c));
  INSERT INTO stage.check_overlap_b_entities SELECT * FROM stage.check_overlap_a_entities;
  INSERT INTO stage.check_overlap_b_paths SELECT * FROM stage.check_overlap_a_paths;
}
teardown
{
  DROP TABLE stage.check_overlap_a_entities,stage.check_overlap_a_paths,
    stage.check_overlap_b_entities,stage.check_overlap_b_paths;
}
session "first"
step "begin_first" { BEGIN; }
step "publish_first" { SELECT * FROM laplace_merge_stage('stage.check_overlap_a_entities','stage.check_overlap_a_paths'); }
step "rollback_first" { ROLLBACK; }
session "second"
setup { SET application_name='laplace_check_stage_overlap'; }
step "begin_second" { BEGIN; }
step "publish_second" { SELECT * FROM laplace_merge_stage('stage.check_overlap_b_entities','stage.check_overlap_b_paths'); }
step "verify_second" {
  SELECT (SELECT count(*) FROM public.entity WHERE id=laplace_id('lpcheckstageoverlap')) AS entities,
         (SELECT count(*) FROM public.physicality WHERE entity=laplace_id('lpcheckstageoverlap')) AS paths,
         (SELECT count(*) FROM stage.check_overlap_b_entities) AS pending_entities,
         (SELECT count(*) FROM stage.check_overlap_b_paths) AS pending_paths;
}
step "rollback_second" { ROLLBACK; }
session "observer"
step "overlap_wait" {
  SELECT wait_event_type,wait_event FROM pg_stat_activity
  WHERE datname=current_database() AND application_name='laplace_check_stage_overlap';
}
permutation "begin_first" "publish_first" "begin_second" "publish_second" "overlap_wait" "rollback_first" "verify_second" "rollback_second"
