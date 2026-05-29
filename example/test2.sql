CREATE DATABASE shop;
USE shop;

CREATE TABLE products (
    id INT INDEXED,
    name STRING NOT_NULL,
    category STRING DEFAULT "Общая категория",
    price INT DEFAULT 100
);

INSERT INTO products (id, name, category, price) VALUES
(1, "Ноутбук", "Электроника", 1200),
(2, "Смартфон", "Гаджеты", 800),
(3, "Монитор", "Электроника", 300),
(4, "Клавиатура", "Аксессуары", 50),
(5, "Наушники", "Гаджеты", 150),
(6, "Мышь", "Аксессуары", 30),
(7, "Принтер", "Электроника", 250),
(8, "Планшет", "Гаджеты", 450),
(9, "Колонки", "Электроника", 100),
(10, "Зарядка", "Аксессуары", 20);

SELECT * FROM products WHERE id BETWEEN 3 AND 8;


SELECT * FROM products WHERE (category == "Электроника" AND price >= 250) OR (category == "Аксессуары" AND price < 40);


SELECT COUNT(*), SUM(price), AVG(price) FROM products WHERE id >= 1 AND id <= 10;


UPDATE products SET category = "Премиум" WHERE id == 2 OR id == 5;


DELETE FROM products WHERE price < 50 OR name LIKE "П.*";


SELECT * FROM products;

INSERT INTO products (id, name) VALUES (11, "Флешка");


SELECT * FROM products WHERE id == 11;


INSERT INTO products (id, name) VALUES (11, "Дубликат");


INSERT INTO products (id) VALUES (12);