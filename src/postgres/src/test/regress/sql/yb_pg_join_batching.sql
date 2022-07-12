CREATE TABLE p1 (a int, b int, c varchar, primary key(a,b));
INSERT INTO p1 SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 2 = 0;
ANALYZE p1;

CREATE TABLE p2 (a int, b int, c varchar, primary key(a,b));
INSERT INTO p2 SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 3 = 0;
ANALYZE p2;

CREATE TABLE p3 (a int, b int, c varchar, primary key(a,b));
INSERT INTO p3 SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 5 = 0;

CREATE TABLE p4 (a int, b int, c varchar, primary key(a,b));
INSERT INTO p4 SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 7 = 0;
ANALYZE p4;

-- We're testing nested loop join batching in this file
SET enable_hashjoin = off;
SET enable_mergejoin = off;

SET yb_nl_batch_size = 3;

EXPLAIN (COSTS OFF) SELECT * FROM p1 t1 JOIN p2 t2 ON t1.a = t2.a WHERE t1.a <= 100 AND t2.a <= 100;
SELECT * FROM p1 t1 JOIN p2 t2 ON t1.a = t2.a WHERE t1.a <= 100 AND t2.a <= 100;

EXPLAIN (COSTS OFF) SELECT * FROM p3 t3 LEFT OUTER JOIN (SELECT t1.a as a FROM p1 t1 JOIN p2 t2 ON t1.a = t2.b WHERE t1.a <= 100 AND t2.a <= 100) s ON t3.a = s.a WHERE t3.a <= 30;
SELECT * FROM p3 t3 LEFT OUTER JOIN (SELECT t1.a as a FROM p1 t1 JOIN p2 t2 ON t1.a = t2.b WHERE t1.a <= 100 AND t2.a <= 100) s ON t3.a = s.a WHERE t3.a <= 30;

EXPLAIN (COSTS OFF) SELECT * FROM p3 t3 RIGHT OUTER JOIN (SELECT t1.a as a FROM p1 t1 JOIN p2 t2 ON t1.a = t2.b WHERE t1.b <= 10 AND t2.b <= 15) s ON t3.a = s.a;
SELECT * FROM p3 t3 RIGHT OUTER JOIN (SELECT t1.a as a FROM p1 t1 JOIN p2 t2 ON t1.a = t2.b WHERE t1.b <= 10 AND t2.b <= 15) s ON t3.a = s.a;

-- anti join--
EXPLAIN (COSTS OFF) SELECT * FROM p1 t1 WHERE NOT EXISTS (SELECT 1 FROM p2 t2 WHERE t1.a = t2.a) AND t1.a <= 40;
SELECT * FROM p1 t1 WHERE NOT EXISTS (SELECT 1 FROM p2 t2 WHERE t1.a = t2.a) AND t1.a <= 40;

-- semi join--
EXPLAIN (COSTS OFF) SELECT * FROM p1 t1 WHERE EXISTS (SELECT 1 FROM p2 t2 WHERE t1.a = t2.a) AND t1.a <= 40;
SELECT * FROM p1 t1 WHERE EXISTS (SELECT 1 FROM p2 t2 WHERE t1.a = t2.a) AND t1.a <= 40;

set yb_nl_batch_size to 10;
set enable_seqscan to false;
explain (costs off) select * from p1 a join p2 b on a.a = b.a join p3 c on b.a = c.a join p4 d on a.b = d.b where a.b = 10 ORDER BY a.a, b.a, c.a, d.a;
select * from p1 a join p2 b on a.a = b.a join p3 c on b.a = c.a join p4 d on a.b = d.b where a.b = 10 ORDER BY a.a, b.a, c.a, d.a;

DROP TABLE p1;
DROP TABLE p2;
DROP TABLE p3;
DROP TABLE p4;
