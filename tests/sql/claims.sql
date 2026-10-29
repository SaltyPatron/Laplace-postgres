-- Selected claims share Native set access, including qualification before fan.
BEGIN;
CREATE TEMP TABLE selected_claim_fixture AS
SELECT label,ids,laplace_compose(ids) id FROM
 (VALUES('ab',ARRAY[97,98]),('acb',ARRAY[97,99,98]),('adb',ARRAY[97,100,98]),
        ('abb',ARRAY[97,98,98]),('af',ARRAY[97,102])) v(label,codepoints)
CROSS JOIN LATERAL (SELECT array_agg(laplace_cp_id(cp) ORDER BY ordinal) ids
 FROM unnest(codepoints) WITH ORDINALITY u(cp,ordinal)) p;
INSERT INTO entity(id,tier,coord,hilbert)
SELECT id,laplace_tier(label),laplace_coord(label),laplace_hilbert(label) FROM selected_claim_fixture;
INSERT INTO physicality(entity,tier,hilbert,path,mask)
SELECT f.id,e.tier,e.hilbert,laplace_path(ids),laplace_mask_slot('kind','claim')
FROM selected_claim_fixture f JOIN entity e ON e.id=f.id;
-- An unscored claim exists but cannot consume a scored responder's place.
INSERT INTO consensus SELECT id,1550,60,0.06,5000000001 FROM selected_claim_fixture WHERE label<>'af';
DO $$
DECLARE a blake3:=laplace_cp_id(97); b blake3:=laplace_cp_id(98); c blake3:=laplace_cp_id(99);
        keys blake3[];bits smallint[]:=ARRAY[laplace_bank_bit('kind','claim')];expected blake3;
BEGIN
 keys:=ARRAY[a,'00000000000000000000000000000000'::blake3,a,b];
 IF (SELECT count(*) FROM laplace_claims(ARRAY[a,b],NULL,bits,'{}'))<>4
 OR (SELECT count(*) FROM laplace_claims_each(keys,NULL,bits,'{}'))<>12
 OR (SELECT count(*) FROM laplace_claims_each(keys,2,bits,'{}'))<>6
 OR EXISTS(SELECT FROM laplace_claims_each(keys,NULL,bits,'{}') WHERE i=2 OR matches<>5000000001 OR rating<>1550 OR deviation<>60 OR volatility<>0.06)
 OR EXISTS(SELECT FROM laplace_claims_each(keys,0,bits,'{}')) THEN
  RAISE EXCEPTION 'Native scored claim selection lost complete sets, ordinals, fan or typed standings';
 END IF;
 SELECT id INTO expected FROM selected_claim_fixture WHERE label<>'af' ORDER BY id LIMIT 1;
 IF (SELECT entity FROM laplace_claims(ARRAY[a,b],1,bits,'{}')) IS DISTINCT FROM expected
 OR EXISTS(SELECT FROM laplace_claims_each(keys,1,bits,'{}') WHERE entity<>expected) THEN
  RAISE EXCEPTION 'Native bounded claims depend on heap order or count an unscored row';
 END IF;
 SELECT id INTO expected FROM selected_claim_fixture WHERE label NOT IN('af','acb') ORDER BY id LIMIT 1;
 IF (SELECT count(*) FROM laplace_claims_each(keys,NULL,bits,ARRAY[c]))<>9
 OR EXISTS(SELECT FROM laplace_claims_each(keys,1,bits,ARRAY[c]) WHERE entity<>expected)
 OR (SELECT count(*) FROM laplace_claims(ARRAY[a,b],NULL,bits,ARRAY[b]))<>3
 OR EXISTS(SELECT FROM laplace_claims(ARRAY[a,b],NULL,ARRAY[63::smallint],'{}')) THEN
  RAISE EXCEPTION 'Native middle/RLE refusal or mask qualification occurred after fan';
 END IF;
 IF EXISTS(SELECT FROM laplace_claims('{}'::blake3[],NULL,bits,'{}'))
 OR EXISTS(SELECT FROM laplace_claims_each(NULL,NULL,bits,'{}'))
 OR EXISTS(SELECT FROM laplace_claims_each(keys,NULL,NULL,'{}')) THEN
  RAISE EXCEPTION 'Native empty/null claim selection changed';
 END IF;
END $$;
ROLLBACK;
