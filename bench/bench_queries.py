"""Query benchmark for a Laplace database (observability, not tests).

Every query computes its IDs and coordinates in place with the laplace functions (IMMUTABLE, folded at plan time), so
the database is only asked to find and count. Each query runs once cold (PostgreSQL's buffers evicted) and then
several times warm; the report shows the server's execution time from EXPLAIN ANALYZE, the client round trip, the
buffers read and hit, the partitions the plan touched, and the first rows of the result.

Usage: python3 bench_queries.py [conninfo] [warm_runs]
"""
import json, statistics, sys, time
import psycopg2

DSN = sys.argv[1] if len(sys.argv) > 1 else "host=localhost port=5432 user=laplace dbname=laplace"
RUNS = int(sys.argv[2]) if len(sys.argv) > 2 else 7
con = psycopg2.connect(DSN); con.autocommit = True; cur = con.cursor()

def ids(*words): return "ARRAY[" + ", ".join(f"laplace_text_id({w!r})" for w in words) + "]"

Q = [
    ("lookup a word by computed ID, every partition",
     "SELECT e.tier, s.parents, s.occurrences FROM entity e JOIN entity_stats s ON s.id = e.id AND s.tier = e.tier "
     "WHERE e.id = laplace_text_id('Holmes')"),
    ("lookup a word by computed ID and Hilbert key, one partition",
     "SELECT e.tier, s.parents, s.occurrences FROM entity e JOIN entity_stats s ON s.id = e.id AND s.tier = e.tier "
     "WHERE e.tier = 2 AND e.hilbert = laplace_text_hilbert('Holmes') AND e.id = laplace_text_id('Holmes')"),
    ("a word that was never recorded",
     "SELECT count(*) FROM entity WHERE tier = 2 AND hilbert = laplace_text_hilbert('Xyzzyq') AND id = laplace_text_id('Xyzzyq')"),
    ("every container of 'Holmes' (GIN)",
     f"SELECT count(*) FROM physicality WHERE laplace_vertex_ids(path) @> {ids('Holmes')}"),
    ("the run 'Sherlock Holmes' inside containers",
     f"SELECT count(*) FROM physicality p, unnest(laplace_follows(p.path, {ids('Sherlock', ' ')})) f "
     f"WHERE laplace_vertex_ids(p.path) @> {ids('Sherlock', ' ', 'Holmes')} AND f = laplace_text_id('Holmes')"),
    ("what follows 'the capital of '",
     f"SELECT f, count(*) FROM physicality p, unnest(laplace_follows(p.path, {ids('the', ' ', 'capital', ' ', 'of', ' ')})) f "
     f"WHERE laplace_vertex_ids(p.path) @> {ids('the', ' ', 'capital', ' ', 'of')} GROUP BY f ORDER BY 2 DESC LIMIT 12"),
    ("what fills '[Captain, ' ', ?]' across the corpus",
     f"SELECT f, count(*) FROM physicality p, unnest(laplace_follows(p.path, {ids('Captain', ' ')})) f "
     f"WHERE laplace_vertex_ids(p.path) @> {ids('Captain', ' ')} GROUP BY f ORDER BY 2 DESC LIMIT 12"),
    ("the 16 word segments nearest 'king' in 4D (GiST)",
     "SELECT id, coord <<->> laplace_text_coord('king') AS d FROM entity WHERE tier = 2 "
     "ORDER BY coord <<->> laplace_text_coord('king') LIMIT 16"),
    ("the 20 most frequent word segments",
     "SELECT id, occurrences FROM entity_stats WHERE tier = 2 ORDER BY occurrences DESC LIMIT 20"),
]

WORDS = ["the", "a", "an", "his", "one", "Italy", "Armenia", "Ahab", "Peleg", "Bildad", "Sleet", "Pollard", "Mayhew",
         "Scoresby", "Boomer", "Butler", "of", "and", "to", "in", "that", "king", "gin", "nig", "ing", "Holmes"]
cur.execute("SELECT " + ", ".join(f"laplace_text_id(%s)" for _ in WORDS), WORDS)
NAME = {str(u): w for u, w in zip(cur.fetchone(), WORDS)}

def walk(plan, acc):
    acc["read"] += plan.get("Shared Read Blocks", 0); acc["hit"] += plan.get("Shared Hit Blocks", 0)
    if "Relation Name" in plan: acc["rels"].add(plan["Relation Name"])
    for p in plan.get("Plans", []): walk(p, acc)
    return acc

def explain(sql):
    cur.execute("EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) " + sql)
    j = cur.fetchone()[0][0]; acc = walk(j["Plan"], {"read": 0, "hit": 0, "rels": set()})
    return j["Planning Time"], j["Execution Time"], acc

cur.execute("SELECT laplace_isa(), current_setting('io_method'), current_setting('shared_buffers'), version()")
isa, io, sb, ver = cur.fetchone()
print(f"{ver.split(',')[0]}; io_method {io}; shared_buffers {sb}; {isa}\n")
print(f"{'query':<58} {'cold exec':>10} {'warm exec':>10} {'round trip':>11} {'plan':>7} {'read':>7} {'hit':>8} {'parts':>6}")
for title, sql in Q:
    cur.execute("SELECT count(*) FROM pg_buffercache_evict_all()")
    _, cold, accc = explain(sql)
    warm, rtt, plan = [], [], []
    for _ in range(RUNS):
        pl, ex, acc = explain(sql); warm.append(ex); plan.append(pl)
        t = time.perf_counter(); cur.execute(sql); rows = cur.fetchall(); rtt.append((time.perf_counter() - t) * 1000)
    print(f"{title:<58} {cold:8.2f}ms {statistics.median(warm):8.2f}ms {statistics.median(rtt):9.2f}ms "
          f"{statistics.median(plan):5.2f}ms {accc['read']:>7,} {acc['hit']:>8,} {len(acc['rels']):>6}")
    shown = ", ".join(" ".join(NAME.get(str(v), str(v)[:8]) if not isinstance(v, float) else f"{v:.4f}" for v in r) for r in rows[:8])
    print(f"   -> {shown}")
