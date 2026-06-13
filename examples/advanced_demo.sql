CREATE TABLE students (
  id INT PRIMARY KEY,
  name TEXT NOT NULL,
  department TEXT NOT NULL,
  semester INT,
  cgpa INT
);

CREATE INDEX idx_students_department ON students(department) USING HASH;
CREATE INDEX idx_students_cgpa ON students(cgpa) USING BTREE;
CREATE INDEX idx_students_semester ON students(semester) USING BTREE;

INSERT INTO students VALUES (101, 'Aaditya', 'ICT', 6, 9);
INSERT INTO students VALUES (102, 'Mira', 'CSE', 6, 8);
INSERT INTO students VALUES (103, 'Kabir', 'ICT', 4, 7);
INSERT INTO students VALUES (104, 'Riya', 'ECE', 6, 9);
INSERT INTO students VALUES (105, 'Dev', 'ICT', 2, 6);

SHOW TABLES;
DESCRIBE students;

EXPLAIN SELECT id, name, cgpa FROM students WHERE cgpa >= 8 AND semester = 6 ORDER BY cgpa DESC LIMIT 3;
SELECT id, name, cgpa FROM students WHERE cgpa >= 8 AND semester = 6 ORDER BY cgpa DESC LIMIT 3;
SELECT COUNT(*) FROM students WHERE department = 'ICT';
SELECT AVG(cgpa) FROM students WHERE semester >= 4;

BEGIN;
UPDATE students SET cgpa = 10 WHERE id = 101;
SELECT id, name, cgpa FROM students WHERE id = 101;
ROLLBACK;
SELECT id, name, cgpa FROM students WHERE id = 101;

BEGIN;
UPDATE students SET cgpa = 10 WHERE id = 101;
DELETE FROM students WHERE department = 'ECE';
COMMIT;

SELECT * FROM students ORDER BY id ASC;
VACUUM students;
