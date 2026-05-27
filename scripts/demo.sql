CREATE DATABASE demo;
USE demo;
CREATE TABLE users (
    id INT INDEXED,
    name STRING NOT_NULL,
    city STRING DEFAULT "Город не указан",
    age INT DEFAULT 18
);
INSERT INTO users (id, name, city, age) VALUES
    (1, "Алиса", "Берлин", 21),
    (2, "Борис", "Париж", 19),
    (3, "Клара", "Рим", 30),
    (4, "Даниил", "Берлин", 17);
SELECT * FROM users;
SELECT (name AS username, city) FROM users WHERE id == 2;
SELECT * FROM users WHERE (city == "Берлин" AND age >= 18) OR name == "Борис";
SELECT COUNT(*), SUM(age), AVG(age) FROM users WHERE age >= 18;
UPDATE users SET city = "Мадрид" WHERE name == "Борис";
DELETE FROM users WHERE age < 18;
SELECT * FROM users;
