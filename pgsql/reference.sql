
DROP TABLE IF EXISTS students;

CREATE TABLE students (
  id INT PRIMARY KEY,
  name TEXT NOT NULL,
  department TEXT NOT NULL,
  semester INT,
  cgpa INT
);

CREATE INDEX idx_students_department ON students USING HASH (department);
CREATE INDEX idx_students_cgpa ON students USING BTREE (cgpa);
CREATE INDEX idx_students_semester ON students USING BTREE (semester);

INSERT INTO students VALUES
  (101, 'Aaditya', 'ICT', 6, 9),
  (102, 'Mira', 'CSE', 6, 8),
  (103, 'Kabir', 'ICT', 4, 7),
  (104, 'Riya', 'ECE', 6, 9),
  (105, 'Dev', 'ICT', 2, 6);

EXPLAIN ANALYZE
SELECT id, name, cgpa
FROM students
WHERE cgpa >= 8 AND semester = 6
ORDER BY cgpa DESC
LIMIT 3;

BEGIN;
UPDATE students SET cgpa = 10 WHERE id = 101;
ROLLBACK;

BEGIN;
UPDATE students SET cgpa = 10 WHERE id = 101;
DELETE FROM students WHERE department = 'ECE';
COMMIT;

SELECT department, COUNT(*), AVG(cgpa)
FROM students
GROUP BY department
ORDER BY department;
