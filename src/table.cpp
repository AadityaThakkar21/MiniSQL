#include "minidb.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using namespace std;

namespace minidb {

// Forward declarations - implementations in minidb.cpp
extern string lower(string text);
extern string upper(string text);
extern bool isInteger(const string& value);
extern FieldType parseType(const string& text);
extern string typeName(FieldType type);
extern IndexKind parseIndexKind(const string& text);
extern string indexKindName(IndexKind kind);
extern string encode(const string& value);
extern string decode(const string& value);
extern vector<string> splitTsv(const string& line);
extern vector<string> splitComma(const string& text);
extern string unquote(string value);
extern vector<Predicate> parsePredicates(const string& text);
extern optional<Aggregate> parseAggregate(const vector<string>& projection);
extern string formatRows(const vector<string>& headers, const vector<vector<string>>& rows, const string& plan);

Table::Table(string name, vector<Column> columns)
    : tableName_(lower(move(name))), columns_(move(columns)) {
  for (auto& column : columns_) {
    column.name = lower(column.name);
    if (column.primaryKey) column.notNull = true;
  }
  if (primaryKeyIndex() >= 0) {
    createIndex("pk_" + tableName_, columns_[static_cast<size_t>(primaryKeyIndex())].name, IndexKind::BTree);
  }
}

const string& Table::name() const { return tableName_; }
const vector<Column>& Table::columns() const { return columns_; }
const vector<Index>& Table::indexes() const { return indexes_; }

int Table::columnIndex(const string& column) const {
  const auto normalized = lower(column);
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].name == normalized) return static_cast<int>(i);
  }
  return -1;
}

int Table::primaryKeyIndex() const {
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].primaryKey) return static_cast<int>(i);
  }
  return -1;
}

string Table::indexKey(const string& column, const string& value) const {
  const int col = columnIndex(column);
  if (col < 0) throw runtime_error("unknown indexed column: " + column);
  if (columns_[static_cast<size_t>(col)].type == FieldType::Text) return value;
  if (!isInteger(value)) throw runtime_error("expected INT for indexed column " + column);
  const auto numeric = static_cast<unsigned long long>(stoll(value)) ^ (1ULL << 63U);
  ostringstream key;
  key << setw(20) << setfill('0') << numeric;
  return key.str();
}

void Table::validateRow(const Row& values) const {
  if (values.size() != columns_.size()) {
    throw runtime_error("column count mismatch for table " + tableName_);
  }
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].notNull && values[i].empty()) {
      throw runtime_error("NOT NULL violation on column " + columns_[i].name);
    }
    if (columns_[i].type == FieldType::Int && !isInteger(values[i])) {
      throw runtime_error("expected INT for column " + columns_[i].name);
    }
  }
}

void Table::validatePrimaryKeyUniqueness(const Row& values, optional<size_t> skip) const {
  const int pk = primaryKeyIndex();
  if (pk < 0) return;
  const auto& key = values[static_cast<size_t>(pk)];
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (skip.has_value() && i == skip.value()) continue;
    if (rows_[i].has_value() && rows_[i].value()[static_cast<size_t>(pk)] == key) {
      throw runtime_error("duplicate primary key");
    }
  }
}

void Table::createIndex(const string& indexName, const string& column, IndexKind kind) {
  const int col = columnIndex(column);
  if (col < 0) throw runtime_error("unknown column in index: " + column);
  const auto normalized = lower(indexName);
  for (const auto& index : indexes_) {
    if (index.name == normalized) throw runtime_error("index already exists: " + indexName);
  }
  indexes_.push_back(Index{normalized, columns_[static_cast<size_t>(col)].name, kind, {}, {}});
  rebuildIndexes();
}

void Table::addToIndexes(size_t rowId, const Row& row) {
  for (auto& index : indexes_) {
    const int col = columnIndex(index.column);
    index.add(indexKey(index.column, row[static_cast<size_t>(col)]), rowId);
  }
}

void Table::removeFromIndexes(size_t rowId, const Row& row) {
  for (auto& index : indexes_) {
    const int col = columnIndex(index.column);
    index.remove(indexKey(index.column, row[static_cast<size_t>(col)]), rowId);
  }
}

void Table::rebuildIndexes() {
  for (auto& index : indexes_) index.clear();
  for (size_t rowId = 0; rowId < rows_.size(); ++rowId) {
    if (rows_[rowId].has_value()) addToIndexes(rowId, rows_[rowId].value());
  }
}

string Table::insert(vector<string> values) {
  for (auto& value : values) value = unquote(value);
  validateRow(values);
  validatePrimaryKeyUniqueness(values, nullopt);
  rows_.push_back(values);
  addToIndexes(rows_.size() - 1, rows_.back().value());
  return "INSERT 1";
}

bool Table::matches(size_t rowId, const vector<Predicate>& predicates) const {
  if (!rows_[rowId].has_value()) return false;
  for (const auto& predicate : predicates) {
    const int col = columnIndex(predicate.column);
    if (col < 0) throw runtime_error("unknown column in WHERE: " + predicate.column);
    const auto& column = columns_[static_cast<size_t>(col)];
    const auto& actual = rows_[rowId].value()[static_cast<size_t>(col)];
    if (column.type == FieldType::Int) {
      if (!isInteger(predicate.value)) {
        throw runtime_error("expected INT in predicate for column " + column.name);
      }
      const long long a = stoll(actual);
      const long long b = stoll(predicate.value);
      if (predicate.op == "=" && !(a == b)) return false;
      if (predicate.op == "!=" && !(a != b)) return false;
      if (predicate.op == "<" && !(a < b)) return false;
      if (predicate.op == "<=" && !(a <= b)) return false;
      if (predicate.op == ">" && !(a > b)) return false;
      if (predicate.op == ">=" && !(a >= b)) return false;
    } else {
      if (predicate.op == "=" && !(actual == predicate.value)) return false;
      if (predicate.op == "!=" && !(actual != predicate.value)) return false;
      if (predicate.op == "<" && !(actual < predicate.value)) return false;
      if (predicate.op == "<=" && !(actual <= predicate.value)) return false;
      if (predicate.op == ">" && !(actual > predicate.value)) return false;
      if (predicate.op == ">=" && !(actual >= predicate.value)) return false;
    }
  }
  return true;
}

QueryPlan Table::plan(const vector<Predicate>& predicates) const {
  QueryPlan best;
  best.description = "PLAN: sequential scan";

  for (const auto& predicate : predicates) {
    const int col = columnIndex(predicate.column);
    if (col < 0) throw runtime_error("unknown column in WHERE: " + predicate.column);
    for (const auto& index : indexes_) {
      if (index.column != columns_[static_cast<size_t>(col)].name) continue;
      if (predicate.op != "=" && (index.kind != IndexKind::BTree || predicate.op == "!=")) continue;
      set<size_t> ids;
      const auto key = indexKey(index.column, predicate.value);
      if (predicate.op == "=") ids = index.equality(key);
      else ids = index.range(predicate.op, key);
      if (best.description == "PLAN: sequential scan" || ids.size() < best.candidateRowIds.size()) {
        best.candidateRowIds.assign(ids.begin(), ids.end());
        best.description = "PLAN: index scan using " + index.name + " (" + indexKindName(index.kind) +
                           ") on " + index.column;
      }
    }
  }

  if (best.description == "PLAN: sequential scan") {
    for (size_t rowId = 0; rowId < rows_.size(); ++rowId) {
      if (rows_[rowId].has_value()) best.candidateRowIds.push_back(rowId);
    }
  }

  vector<size_t> filtered;
  for (const auto rowId : best.candidateRowIds) {
    ++best.rowsExamined;
    if (matches(rowId, predicates)) filtered.push_back(rowId);
  }
  best.candidateRowIds = move(filtered);
  best.description += " | rows_examined=" + to_string(best.rowsExamined) +
                      " | rows_returned=" + to_string(best.candidateRowIds.size());
  return best;
}

string Table::aggregate(const SelectQuery& query, const vector<size_t>& rowIds,
                       const string& planText) const {
  const auto& agg = query.aggregate.value();
  if (agg.function == "COUNT") {
    return formatRows({"count"}, {{to_string(rowIds.size())}}, planText);
  }
  const int col = columnIndex(agg.column);
  if (col < 0) throw runtime_error("unknown aggregate column: " + agg.column);
  if (rows_.empty() || rowIds.empty()) {
    return formatRows({lower(agg.function) + "(" + agg.column + ")"}, {{""}}, planText);
  }
  const bool numeric = columns_[static_cast<size_t>(col)].type == FieldType::Int;
  if (agg.function == "AVG" && !numeric) throw runtime_error("AVG requires INT column");

  long long sum = 0;
  string best;
  bool first = true;
  for (const auto id : rowIds) {
    const auto& value = rows_[id].value()[static_cast<size_t>(col)];
    if (numeric) {
      const long long n = stoll(value);
      sum += n;
      if (first || (agg.function == "MIN" && n < stoll(best)) ||
          (agg.function == "MAX" && n > stoll(best))) {
        best = value;
      }
    } else if (first || (agg.function == "MIN" && value < best) ||
               (agg.function == "MAX" && value > best)) {
      best = value;
    }
    first = false;
  }
  if (agg.function == "AVG") {
    ostringstream avg;
    avg << fixed << setprecision(2)
        << static_cast<double>(sum) / static_cast<double>(rowIds.size());
    best = avg.str();
  }
  return formatRows({lower(agg.function) + "(" + agg.column + ")"}, {{best}}, planText);
}

string Table::select(const SelectQuery& query) const {
  auto planned = plan(query.predicates);
  if (query.explainOnly) return planned.description;
  if (query.aggregate.has_value()) {
    return aggregate(query, planned.candidateRowIds, planned.description);
  }

  vector<int> selectedColumns;
  vector<string> headers;
  if (query.projection.size() >= 1 && trim(query.projection[0]) == "*") {
    for (size_t i = 0; i < columns_.size(); ++i) {
      selectedColumns.push_back(static_cast<int>(i));
      headers.push_back(columns_[i].name);
    }
  } else {
    for (const auto& column : query.projection) {
      if (column.rfind("__table__", 0) == 0) continue;
      const int col = columnIndex(column);
      if (col < 0) throw runtime_error("unknown selected column: " + column);
      selectedColumns.push_back(col);
      headers.push_back(columns_[static_cast<size_t>(col)].name);
    }
  }

  auto ids = planned.candidateRowIds;
  if (query.orderBy.has_value()) {
    const int orderCol = columnIndex(query.orderBy->column);
    if (orderCol < 0) throw runtime_error("unknown ORDER BY column: " + query.orderBy->column);
    sort(ids.begin(), ids.end(), [&](size_t left, size_t right) {
      const auto& a = rows_[left].value()[static_cast<size_t>(orderCol)];
      const auto& b = rows_[right].value()[static_cast<size_t>(orderCol)];
      if (columns_[static_cast<size_t>(orderCol)].type == FieldType::Int) {
        const auto leftValue = stoll(a);
        const auto rightValue = stoll(b);
        return query.orderBy->direction == SortDirection::Asc ? leftValue < rightValue : leftValue > rightValue;
      }
      return query.orderBy->direction == SortDirection::Asc ? a < b : a > b;
    });
  }
  if (query.limit.has_value() && ids.size() > query.limit.value()) {
    ids.resize(query.limit.value());
  }

  vector<vector<string>> resultRows;
  for (const auto id : ids) {
    vector<string> row;
    for (const auto col : selectedColumns) {
      row.push_back(rows_[id].value()[static_cast<size_t>(col)]);
    }
    resultRows.push_back(row);
  }
  return formatRows(headers, resultRows, planned.description);
}

string Table::update(const vector<pair<string, string>>& assignments,
                    const vector<Predicate>& predicates) {
  auto planned = plan(predicates);
  size_t changed = 0;
  for (const auto id : planned.candidateRowIds) {
    auto next = rows_[id].value();
    for (const auto& [column, value] : assignments) {
      const int col = columnIndex(column);
      if (col < 0) throw runtime_error("unknown column in SET: " + column);
      next[static_cast<size_t>(col)] = unquote(value);
    }
    validateRow(next);
    validatePrimaryKeyUniqueness(next, id);
    removeFromIndexes(id, rows_[id].value());
    rows_[id] = next;
    addToIndexes(id, rows_[id].value());
    ++changed;
  }
  return planned.description + "\nUPDATE " + to_string(changed);
}

string Table::erase(const vector<Predicate>& predicates) {
  auto planned = plan(predicates);
  size_t removed = 0;
  for (const auto id : planned.candidateRowIds) {
    if (rows_[id].has_value()) {
      removeFromIndexes(id, rows_[id].value());
      rows_[id].reset();
      ++removed;
    }
  }
  return planned.description + "\nDELETE " + to_string(removed);
}

string Table::describe() const {
  ostringstream out;
  out << "table: " << tableName_ << "\ncolumns:\n";
  for (const auto& column : columns_) {
    out << "  " << column.name << " " << typeName(column.type);
    if (column.primaryKey) out << " PRIMARY KEY";
    if (column.notNull && !column.primaryKey) out << " NOT NULL";
    out << "\n";
  }
  out << "indexes:\n";
  for (const auto& index : indexes_) {
    out << "  " << index.name << " ON " << index.column << " USING " << indexKindName(index.kind) << "\n";
  }
  return out.str();
}

string Table::vacuum() {
  const auto before = rows_.size();
  vector<optional<Row>> compacted;
  for (const auto& row : rows_) {
    if (row.has_value()) compacted.push_back(row);
  }
  rows_ = move(compacted);
  rebuildIndexes();
  return "VACUUM removed " + to_string(before - rows_.size()) + " dead row slot(s)";
}

void Table::save(const filesystem::path& dataDir) const {
  filesystem::create_directories(dataDir);
  ofstream schema(dataDir / (tableName_ + ".schema"));
  schema << "name=" << tableName_ << "\ncolumns=";
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (i) schema << ",";
    schema << columns_[i].name << ":" << typeName(columns_[i].type);
    if (columns_[i].primaryKey) schema << ":PK";
    else if (columns_[i].notNull) schema << ":NN";
  }
  schema << "\nindexes=";
  for (size_t i = 0; i < indexes_.size(); ++i) {
    if (i) schema << ",";
    schema << indexes_[i].name << ":" << indexes_[i].column << ":" << indexKindName(indexes_[i].kind);
  }
  schema << "\n";

  ofstream rows(dataDir / (tableName_ + ".rows"));
  for (const auto& row : rows_) {
    rows << (row.has_value() ? "1" : "0");
    if (row.has_value()) {
      for (const auto& value : row.value()) rows << '\t' << encode(value);
    }
    rows << "\n";
  }
}

Table Table::load(const filesystem::path& schemaPath) {
  ifstream schema(schemaPath);
  if (!schema) throw runtime_error("could not read schema: " + schemaPath.string());

  string tableName;
  vector<Column> columns;
  vector<Index> indexes;
  string line;
  while (getline(schema, line)) {
    const auto pos = line.find('=');
    if (pos == string::npos) continue;
    const auto key = line.substr(0, pos);
    const auto value = line.substr(pos + 1);
    if (key == "name") {
      tableName = value;
    } else if (key == "columns") {
      for (const auto& spec : splitComma(value)) {
        vector<string> parts;
        stringstream ss(spec);
        string token;
        while (getline(ss, token, ':')) parts.push_back(token);
        if (parts.size() < 2) throw runtime_error("bad column spec in schema");
        columns.push_back(Column{lower(parts[0]), parseType(parts[1]),
                                 parts.size() >= 3 && upper(parts[2]) == "PK",
                                 parts.size() >= 3 && upper(parts[2]) == "NN"});
      }
    } else if (key == "indexes" && !trim(value).empty()) {
      for (const auto& spec : splitComma(value)) {
        vector<string> parts;
        stringstream ss(spec);
        string token;
        while (getline(ss, token, ':')) parts.push_back(token);
        if (parts.size() != 3) throw runtime_error("bad index spec in schema");
        indexes.push_back(Index{lower(parts[0]), lower(parts[1]), parseIndexKind(parts[2]), {}, {}});
      }
    }
  }

  Table table(tableName, columns);
  table.indexes_ = indexes;

  ifstream rows(schemaPath.parent_path() / (tableName + ".rows"));
  while (getline(rows, line)) {
    const auto parts = splitTsv(line);
    if (parts.empty()) continue;
    if (parts[0] == "0") {
      table.rows_.push_back(nullopt);
    } else {
      Row row(parts.begin() + 1, parts.end());
      table.validateRow(row);
      table.rows_.push_back(row);
    }
  }
  table.rebuildIndexes();
  return table;
}

} // namespace minidb
