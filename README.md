# MiniSQL

A lightweight SQL database engine written in C++17. Think of it as a mini version of SQLite - it parses SQL, stores data in files, and uses indexes to speed up queries.

## What It Does

MiniSQL lets you create tables, insert data, and query it using actual SQL commands. It has real database features like B-tree and hash indexes for fast lookups, transactions with BEGIN/COMMIT/ROLLBACK, query planning that picks the best index to use, aggregate functions like COUNT/AVG/MIN/MAX, and file-based storage that persists between runs.

The cool part? When you run a query like WHERE cgpa >= 8, it doesn't scan every row. It uses a B-tree index to jump straight to the relevant data, just like PostgreSQL or MySQL would.

## How It Works

The engine follows a classic database architecture: SQL text goes through a Parser, then Validator, then Query Planner, then Executor, and finally File Storage.

Indexes: You can create two types - B-tree indexes using std::map for range queries like >= and <=, and Hash indexes using std::unordered_map for exact matches.

Storage: Tables are stored as two files - a .schema file with column definitions and index metadata, and a .rows file with actual row data in tab-separated format.

Query Planning: The planner looks at your WHERE clause and picks the index that returns the fewest rows. This turns an O(n) table scan into an O(log n) index lookup.

## Running It

Build with CMake:

cmake -S . -B build
cmake --build build

Or compile directly:

g++ -std=c++17 -Wall -Wextra -pedantic -Iinclude src/main.cpp src/minidb.cpp -o build/minisql.exe

Run with a SQL file:

.\\build\\minisql.exe --data demo-data --file examples\\advanced_demo.sql

Or use the interactive shell:

.\\build\\minisql.exe

