CREATE TABLE p1 (a int, b int, c varchar, primary key(a,b));
INSERT INTO p1 SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 2 = 0;
ANALYZE p1;

CREATE TABLE p2 (a int, b int, c varchar, primary key(a,b));
INSERT INTO p2 SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 3 = 0;
ANALYZE p2;

CREATE TABLE p3 (a int, b int, c varchar, primary key(a,b));
INSERT INTO p3 SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 5 = 0;
ANALYZE p3;

CREATE TABLE p4 (a int, b int, c varchar, primary key(a,b));
INSERT INTO p4 SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 7 = 0;
ANALYZE p4;

-- We're testing nested loop join batching in this file
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_seqscan = off;

SET yb_bnl_batch_size = 3;

EXPLAIN (COSTS OFF) SELECT * FROM p1 t1 JOIN p2 t2 ON t1.a = t2.a WHERE t1.a <= 100 AND t2.a <= 100;
SELECT * FROM p1 t1 JOIN p2 t2 ON t1.a = t2.a WHERE t1.a <= 100 AND t2.a <= 100;

EXPLAIN (COSTS OFF) SELECT * FROM p3 t3 LEFT OUTER JOIN (SELECT t1.a as a FROM p1 t1 JOIN p2 t2 ON t1.a = t2.b WHERE t1.a <= 100 AND t2.a <= 100) s ON t3.a = s.a WHERE t3.a <= 30;
SELECT * FROM p3 t3 LEFT OUTER JOIN (SELECT t1.a as a FROM p1 t1 JOIN p2 t2 ON t1.a = t2.b WHERE t1.a <= 100 AND t2.a <= 100) s ON t3.a = s.a WHERE t3.a <= 30;

EXPLAIN (COSTS OFF) SELECT * FROM p3 t3 RIGHT OUTER JOIN (SELECT t1.a as a FROM p1 t1 JOIN p2 t2 ON t1.a = t2.b WHERE t1.b <= 10 AND t2.b <= 15) s ON t3.a = s.a;
SELECT * FROM p3 t3 RIGHT OUTER JOIN (SELECT t1.a as a FROM p1 t1 JOIN p2 t2 ON t1.a = t2.b WHERE t1.b <= 10 AND t2.b <= 15) s ON t3.a = s.a;

-- anti join--
EXPLAIN (COSTS OFF) SELECT * FROM p1 t1 WHERE NOT EXISTS (SELECT 1 FROM p2 t2 WHERE t1.a = t2.a) AND t1.a <= 40;
SELECT * FROM p1 t1 WHERE NOT EXISTS (SELECT 1 FROM p2 t2 WHERE t1.a = t2.a) AND t1.a <= 40;

EXPLAIN (COSTS OFF) SELECT * FROM p1 t1 WHERE NOT EXISTS (SELECT 1 FROM p2 t2 WHERE t1.a = t2.b) AND t1.a <= 40;
SELECT * FROM p1 t1 WHERE NOT EXISTS (SELECT 1 FROM p2 t2 WHERE t1.a = t2.b) AND t1.a <= 40;

-- semi join--
EXPLAIN (COSTS OFF) SELECT * FROM p1 t1 WHERE EXISTS (SELECT 1 FROM p2 t2 WHERE t1.a = t2.a) AND t1.a <= 40;
SELECT * FROM p1 t1 WHERE EXISTS (SELECT 1 FROM p2 t2 WHERE t1.a = t2.a) AND t1.a <= 40;

<<<<<<< HEAD
EXPLAIN (COSTS OFF) SELECT * FROM p1 t1 WHERE EXISTS (SELECT 1 FROM p2 t2 WHERE t1.a = t2.b) AND t1.a <= 40;
SELECT * FROM p1 t1 WHERE EXISTS (SELECT 1 FROM p2 t2 WHERE t1.a = t2.b) AND t1.a <= 40;

set yb_bnl_batch_size to 10;
=======
set yb_bnl_batch_size to 10;
set enable_seqscan to false;
>>>>>>> 9c84764340 (bug in tuplestore codepath)
explain (costs off) select * from p1 a join p2 b on a.a = b.a join p3 c on b.a = c.a join p4 d on a.b = d.b where a.b = 10 ORDER BY a.a, b.a, c.a, d.a;
select * from p1 a join p2 b on a.a = b.a join p3 c on b.a = c.a join p4 d on a.b = d.b where a.b = 10 ORDER BY a.a, b.a, c.a, d.a;

DROP TABLE p1;
DROP TABLE p2;
DROP TABLE p3;
DROP TABLE p4;

SELECT '' AS "xxx", *
  FROM J1_TBL AS tx order by 1, 2, 3, 4;

SELECT '' AS "xxx", *
  FROM J1_TBL tx  order by 1, 2, 3, 4;

SELECT '' AS "xxx", *
  FROM J1_TBL AS t1 (a, b, c)  order by 1, 2, 3, 4;

SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b, c)  order by 1, 2, 3, 4;

SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b, c), J2_TBL t2 (d, e)  order by 1, 2, 3, 4, 5, 6;

SELECT '' AS "xxx", t1.a, t2.e
  FROM J1_TBL t1 (a, b, c), J2_TBL t2 (d, e)
  WHERE t1.a = t2.d  order by 1, 2, 3;

--
--
-- Inner joins (equi-joins)
--
--

--
-- Inner joins (equi-joins) with USING clause
-- The USING syntax changes the shape of the resulting table
-- by including a column in the USING clause only once in the result.
--

-- Inner equi-join on specified column
SELECT '' AS "xxx", *
  FROM J1_TBL INNER JOIN J2_TBL USING (i) order by 1, 2, 3, 4, 5;

-- Same as above, slightly different syntax
SELECT '' AS "xxx", *
  FROM J1_TBL JOIN J2_TBL USING (i) order by 1, 2, 3, 4, 5;

SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b, c) JOIN J2_TBL t2 (a, d) USING (a)
  ORDER BY a, d;

SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b, c) JOIN J2_TBL t2 (a, b) USING (b)
  ORDER BY b, t1.a;

--
-- NATURAL JOIN
-- Inner equi-join on all columns with the same name
--

SELECT '' AS "xxx", *
  FROM J1_TBL NATURAL JOIN J2_TBL order by 1, 2, 3, 4, 5;

SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b, c) NATURAL JOIN J2_TBL t2 (a, d) order by 1, 2, 3, 4, 5;

SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b, c) NATURAL JOIN J2_TBL t2 (d, a) order by 1, 2, 3, 4, 5;

-- mismatch number of columns
-- currently, Postgres will fill in with underlying names
SELECT '' AS "xxx", *
  FROM J1_TBL t1 (a, b) NATURAL JOIN J2_TBL t2 (a) order by 1, 2, 3, 4, 5;


--
-- Inner joins (equi-joins)
--

SELECT '' AS "xxx", *
  FROM J1_TBL JOIN J2_TBL ON (J1_TBL.i = J2_TBL.i) order by 1, 2, 3, 4, 5, 6;

SELECT '' AS "xxx", *
  FROM J1_TBL JOIN J2_TBL ON (J1_TBL.i = J2_TBL.k) order by 1, 2, 3, 4, 5, 6;


--
-- Non-equi-joins
--

SELECT '' AS "xxx", *
  FROM J1_TBL JOIN J2_TBL ON (J1_TBL.i <= J2_TBL.k) order by 1, 2, 3, 4, 5, 6;


--
-- Outer joins
-- Note that OUTER is a noise word
--

SELECT '' AS "xxx", *
  FROM J1_TBL LEFT OUTER JOIN J2_TBL USING (i)
  ORDER BY i, k, t;

SELECT '' AS "xxx", *
  FROM J1_TBL LEFT JOIN J2_TBL USING (i)
  ORDER BY i, k, t;

SELECT '' AS "xxx", *
  FROM J1_TBL RIGHT OUTER JOIN J2_TBL USING (i) order by 1, 2, 3, 4, 5;

SELECT '' AS "xxx", *
  FROM J1_TBL RIGHT JOIN J2_TBL USING (i) order by 1, 2, 3, 4, 5;

SELECT '' AS "xxx", *
  FROM J1_TBL FULL OUTER JOIN J2_TBL USING (i)
  ORDER BY i, k, t;

SELECT '' AS "xxx", *
  FROM J1_TBL FULL JOIN J2_TBL USING (i)
  ORDER BY i, k, t;

SELECT '' AS "xxx", *
  FROM J1_TBL LEFT JOIN J2_TBL USING (i) WHERE (k = 1) order by 1, 2, 3, 4, 5;

SELECT '' AS "xxx", *
  FROM J1_TBL LEFT JOIN J2_TBL USING (i) WHERE (i = 1) order by 1, 2, 3, 4, 5;

--
-- semijoin selectivity for <>
--
-- explain (costs off)
-- select * from int4_tbl i4, tenk1 a
-- where exists(select * from tenk1 b
--              where a.twothousand = b.twothousand and a.fivethous <> b.fivethous)
--       and i4.f1 = a.tenthous;


--
-- More complicated constructs
--

--
-- Multiway full join
--

SELECT * FROM t1 FULL JOIN t2 USING (name) FULL JOIN t3 USING (name) order by 1, 2, 3, 4;

--
-- Test interactions of join syntax and subqueries
--

-- Basic cases (we expect planner to pull up the subquery here)
SELECT * FROM
(SELECT * FROM t2) as s2
INNER JOIN
(SELECT * FROM t3) s3
USING (name) order by 1, 2, 3;

SELECT * FROM
(SELECT * FROM t2) as s2
LEFT JOIN
(SELECT * FROM t3) s3
USING (name) order by 1, 2, 3;

SELECT * FROM
(SELECT * FROM t2) as s2
FULL JOIN
(SELECT * FROM t3) s3
USING (name) order by 1, 2, 3;

-- Cases with non-nullable expressions in subquery results;
-- make sure these go to null as expected
SELECT * FROM
(SELECT name, n as s2_n, 2 as s2_2 FROM t2) as s2
NATURAL INNER JOIN
(SELECT name, n as s3_n, 3 as s3_2 FROM t3) s3 order by 1, 2, 3, 4, 5;

SELECT * FROM
(SELECT name, n as s2_n, 2 as s2_2 FROM t2) as s2
NATURAL LEFT JOIN
(SELECT name, n as s3_n, 3 as s3_2 FROM t3) s3 order by 1, 2, 3, 4, 5;

SELECT * FROM
(SELECT name, n as s2_n, 2 as s2_2 FROM t2) as s2
NATURAL FULL JOIN
(SELECT name, n as s3_n, 3 as s3_2 FROM t3) s3 order by 1, 2, 3, 4;

SELECT * FROM
(SELECT name, n as s1_n, 1 as s1_1 FROM t1) as s1
NATURAL INNER JOIN
(SELECT name, n as s2_n, 2 as s2_2 FROM t2) as s2
NATURAL INNER JOIN
(SELECT name, n as s3_n, 3 as s3_2 FROM t3) s3 order by 1, 2, 3, 4, 5, 6, 7;

SELECT * FROM
(SELECT name, n as s1_n, 1 as s1_1 FROM t1) as s1
NATURAL FULL JOIN
(SELECT name, n as s2_n, 2 as s2_2 FROM t2) as s2
NATURAL FULL JOIN
(SELECT name, n as s3_n, 3 as s3_2 FROM t3) s3 order by 1, 2, 3, 4, 5, 6, 7;

SELECT * FROM
(SELECT name, n as s1_n FROM t1) as s1
NATURAL FULL JOIN
  (SELECT * FROM
    (SELECT name, n as s2_n FROM t2) as s2
    NATURAL FULL JOIN
    (SELECT name, n as s3_n FROM t3) as s3
  ) ss2 order by 1, 2, 3, 4;

SELECT * FROM
(SELECT name, n as s1_n FROM t1) as s1
NATURAL FULL JOIN
  (SELECT * FROM
    (SELECT name, n as s2_n, 2 as s2_2 FROM t2) as s2
    NATURAL FULL JOIN
    (SELECT name, n as s3_n FROM t3) as s3
  ) ss2 order by 1, 2, 3, 4, 5;


-- Test for propagation of nullability constraints into sub-joins

create temp table x (x1 int, x2 int);
insert into x values (1,11);
insert into x values (2,22);
insert into x values (3,null);
insert into x values (4,44);
insert into x values (5,null);

create temp table y (y1 int, y2 int);
insert into y values (1,111);
insert into y values (2,222);
insert into y values (3,333);
insert into y values (4,null);

select * from x order by 1, 2;
select * from y order by 1, 2;

select * from x left join y on (x1 = y1 and x2 is not null) order by 1, 2, 3, 4;
select * from x left join y on (x1 = y1 and y2 is not null) order by 1, 2, 3, 4;

select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1) order by 1, 2, 3, 4, 5, 6;
select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1 and x2 is not null) order by 1, 2, 3, 4, 5, 6;
select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1 and y2 is not null) order by 1, 2, 3, 4, 5, 6;
select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1 and xx2 is not null) order by 1, 2, 3, 4, 5, 6;
-- these should NOT give the same answers as above
select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1) where (x2 is not null) order by 1;
select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1) where (y2 is not null) order by 1;
select * from (x left join y on (x1 = y1)) left join x xx(xx1,xx2)
on (x1 = xx1) where (xx2 is not null) order by 1;
