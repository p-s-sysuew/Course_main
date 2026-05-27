CREATE DATABASE stress;
USE stress;
CREATE TABLE people (
    id INT INDEXED,
    name STRING NOT_NULL,
    city STRING DEFAULT "Город не указан",
    age INT DEFAULT 18
);
INSERT INTO people (id, name, city, age) VALUES
(1, "Анна", "Берлин", 20),
(2, "Борис", "Париж", 21),
(3, "Катя", "Берлин", 22),
(4, "Данил", "Рим", 23),
(5, "Ева", "Париж", 24),
(6, "Фёдор", "Берлин", 25),
(7, "Глеб", "Рим", 26),
(8, "Нина", "Париж", 27),
(9, "Иван", "Берлин", 28),
(10, "Яна", "Рим", 29);
SELECT * FROM people WHERE id BETWEEN 3 AND 8;
SELECT * FROM people WHERE (city == "Берлин" AND age >= 22) OR (city == "Рим" AND age < 25);
SELECT COUNT(*), SUM(age), AVG(age) FROM people WHERE id >= 1 AND id <= 10;
UPDATE people SET city = "Мадрид" WHERE id == 2 OR id == 5;
DELETE FROM people WHERE age < 22 OR name LIKE "Я.*";
SELECT * FROM people;
INSERT INTO people (id, name) VALUES (11, "Кира");
SELECT * FROM people WHERE id == 11;
INSERT INTO people (id, name) VALUES (11, "Повтор");
INSERT INTO people (id) VALUES (12);
