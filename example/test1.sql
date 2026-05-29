CREATE DATABASE baseTest1;
USE baseTest1;

CREATE TABLE baseTest1 (id INT,name STRING,age INT
);

INSERT INTO baseTest1 (id, name, age) VALUES
    (1, "Иван", 19),
    (2, "Павел", 20),
    (3, "Фёдор", 19);

SELECT * FROM baseTest1;
