#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace minidb {

enum class FieldType { Int, Text };
enum class IndexKind { BTree, Hash };
enum class SortDirection { Asc, Desc };

struct Column {
  std::string name;
  FieldType type = FieldType::Text;
  bool primaryKey = false;
  bool notNull = false;
};

struct Predicate {
  std::string column;
  std::string op;
  std::string value;
};

struct SortSpec {
  std::string column;
  SortDirection direction = SortDirection::Asc;
};

struct Aggregate {
  std::string function;
  std::string column;
};

struct SelectQuery {
  std::vector<std::string> projection;
  std::vector<Predicate> predicates;
  std::optional<SortSpec> orderBy;
  std::optional<std::size_t> limit;
  std::optional<Aggregate> aggregate;
  bool explainOnly = false;
};

struct Index {
  std::string name;
  std::string column;
  IndexKind kind = IndexKind::BTree;
  std::map<std::string, std::set<std::size_t>> btree;
  std::unordered_map<std::string, std::set<std::size_t>> hash;

  void clear();
  void add(const std::string& key, std::size_t rowId);
  void remove(const std::string& key, std::size_t rowId);
  std::set<std::size_t> equality(const std::string& key) const;
  std::set<std::size_t> range(const std::string& op, const std::string& key) const;
};

struct QueryPlan {
  std::vector<std::size_t> candidateRowIds;
  std::string description;
  std::size_t rowsExamined = 0;
};

class Table {
 public:
  Table() = default;
  Table(std::string name, std::vector<Column> columns);

  const std::string& name() const;
  const std::vector<Column>& columns() const;
  const std::vector<Index>& indexes() const;

  void createIndex(const std::string& indexName, const std::string& column, IndexKind kind);
  std::string insert(std::vector<std::string> values);
  std::string select(const SelectQuery& query) const;
  std::string update(const std::vector<std::pair<std::string, std::string>>& assignments,
                     const std::vector<Predicate>& predicates);
  std::string erase(const std::vector<Predicate>& predicates);
  std::string describe() const;
  std::string vacuum();

  void save(const std::filesystem::path& dataDir) const;
  static Table load(const std::filesystem::path& schemaPath);

 private:
  using Row = std::vector<std::string>;

  std::string tableName_;
  std::vector<Column> columns_;
  std::vector<std::optional<Row>> rows_;
  std::vector<Index> indexes_;

  int columnIndex(const std::string& column) const;
  int primaryKeyIndex() const;
  std::string indexKey(const std::string& column, const std::string& value) const;
  void validateRow(const Row& values) const;
  void validatePrimaryKeyUniqueness(const Row& values, std::optional<std::size_t> skip) const;
  void addToIndexes(std::size_t rowId, const Row& row);
  void removeFromIndexes(std::size_t rowId, const Row& row);
  void rebuildIndexes();
  bool matches(std::size_t rowId, const std::vector<Predicate>& predicates) const;
  QueryPlan plan(const std::vector<Predicate>& predicates) const;
  std::string aggregate(const SelectQuery& query, const std::vector<std::size_t>& rows,
                        const std::string& planText) const;
};

class Database {
 public:
  explicit Database(std::filesystem::path dataDir);

  void load();
  std::string execute(const std::string& sql);

 private:
  std::filesystem::path dataDir_;
  std::unordered_map<std::string, Table> tables_;
  std::optional<std::unordered_map<std::string, Table>> transactionSnapshot_;

  std::string createTable(const std::string& sql);
  std::string createIndex(const std::string& sql);
  std::string insert(const std::string& sql);
  std::string select(const std::string& sql, bool explainOnly);
  std::string update(const std::string& sql);
  std::string erase(const std::string& sql);
  std::string showTables() const;
  std::string describe(const std::string& sql) const;
  std::string vacuum(const std::string& sql);
  std::string begin();
  std::string commit();
  std::string rollback();

  void persistIfAutoCommit(const std::string& tableName);
  void persistAll() const;
  bool inTransaction() const;
  Table& table(const std::string& name);
  const Table& table(const std::string& name) const;
};

std::vector<std::string> splitStatements(const std::string& input);
std::string trim(std::string text);

}  // namespace minidb
