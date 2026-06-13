#include "minidb.hpp"
#include <filesystem>
#include <regex>
#include <sstream>
#include <stdexcept>

using namespace std;

namespace minidb {

Database::Database(filesystem::path dataDir) : dataDir_(move(dataDir)) {}

void Database::load() {
  filesystem::create_directories(dataDir_);
  tables_.clear();
  for (const auto& entry : filesystem::directory_iterator(dataDir_)) {
    if (entry.path().extension() == ".schema") {
      auto loaded = Table::load(entry.path());
      tables_.emplace(loaded.name(), move(loaded));
    }
  }
}

bool Database::inTransaction() const { return transactionSnapshot_.has_value(); }

void Database::persistIfAutoCommit(const string& tableName) {
  if (!inTransaction()) table(tableName).save(dataDir_);
}

void Database::persistAll() const {
  for (const auto& [_, t] : tables_) t.save(dataDir_);
}

Table& Database::table(const string& name) {
  const auto it = tables_.find(lower(name));
  if (it == tables_.end()) throw runtime_error("unknown table: " + name);
  return it->second;
}

const Table& Database::table(const string& name) const {
  const auto it = tables_.find(lower(name));
  if (it == tables_.end()) throw runtime_error("unknown table: " + name);
  return it->second;
}

string Database::execute(const string& sql) {
  const auto clean = trim(sql);
  if (clean.empty()) return "";
  if (startsWithKeyword(clean, "BEGIN")) return begin();
  if (startsWithKeyword(clean, "COMMIT")) return commit();
  if (startsWithKeyword(clean, "ROLLBACK")) return rollback();
  if (startsWithKeyword(clean, "SHOW TABLES")) return showTables();
  if (startsWithKeyword(clean, "DESCRIBE")) return describe(clean);
  if (startsWithKeyword(clean, "VACUUM")) return vacuum(clean);
  if (startsWithKeyword(clean, "CREATE TABLE")) return createTable(clean);
  if (startsWithKeyword(clean, "CREATE INDEX")) return createIndex(clean);
  if (startsWithKeyword(clean, "INSERT")) return insert(clean);
  if (startsWithKeyword(clean, "EXPLAIN")) return select(clean, true);
  if (startsWithKeyword(clean, "SELECT")) return select(clean, false);
  if (startsWithKeyword(clean, "UPDATE")) return update(clean);
  if (startsWithKeyword(clean, "DELETE")) return erase(clean);
  throw runtime_error("unsupported SQL command");
}

string Database::begin() {
  if (inTransaction()) throw runtime_error("transaction already active");
  transactionSnapshot_ = tables_;
  return "BEGIN";
}

string Database::commit() {
  if (!inTransaction()) throw runtime_error("no active transaction");
  persistAll();
  transactionSnapshot_.reset();
  return "COMMIT";
}

string Database::rollback() {
  if (!inTransaction()) throw runtime_error("no active transaction");
  tables_ = transactionSnapshot_.value();
  transactionSnapshot_.reset();
  return "ROLLBACK";
}

string Database::createTable(const string& sql) {
  static const regex createRegex(
      R"(^CREATE\s+TABLE\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([\s\S]*)\)$)",
      regex::icase);
  smatch match;
  if (!regex_match(sql, match, createRegex)) {
    throw runtime_error("syntax: CREATE TABLE name (col INT PRIMARY KEY, col TEXT NOT NULL)");
  }
  const auto name = lower(match[1].str());
  if (tables_.find(name) != tables_.end()) throw runtime_error("table already exists: " + name);

  vector<Column> columns;
  string tablePrimaryKey;
  for (const auto& part : splitComma(match[2].str())) {
    static const regex pkRegex(
        R"(^PRIMARY\s+KEY\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)$)",
        regex::icase);
    smatch pkMatch;
    if (regex_match(part, pkMatch, pkRegex)) {
      tablePrimaryKey = lower(pkMatch[1].str());
      continue;
    }
    stringstream ss(part);
    string columnName, typeText;
    ss >> columnName >> typeText;
    if (columnName.empty() || typeText.empty()) throw runtime_error("bad column definition: " + part);
    string rest;
    getline(ss, rest);
    const auto attributes = upper(rest);
    columns.push_back(Column{lower(columnName), parseType(typeText),
                             attributes.find("PRIMARY KEY") != string::npos,
                             attributes.find("NOT NULL") != string::npos});
  }
  if (!tablePrimaryKey.empty()) {
    bool found = false;
    for (auto& column : columns) {
      if (column.name == tablePrimaryKey) {
        column.primaryKey = true;
        column.notNull = true;
        found = true;
      }
    }
    if (!found) throw runtime_error("PRIMARY KEY references unknown column: " + tablePrimaryKey);
  }

  Table t(name, columns);
  tables_.emplace(name, move(t));
  persistIfAutoCommit(name);
  return "CREATE TABLE";
}

string Database::createIndex(const string& sql) {
  static const regex indexRegex(
      R"(^CREATE\s+INDEX\s+([A-Za-z_][A-Za-z0-9_]*)\s+ON\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)(?:\s+USING\s+(BTREE|HASH))?$)",
      regex::icase);
  smatch match;
  if (!regex_match(sql, match, indexRegex)) {
    throw runtime_error("syntax: CREATE INDEX idx ON table(column) USING BTREE|HASH");
  }
  auto& target = table(match[2].str());
  target.createIndex(match[1].str(), match[3].str(), parseIndexKind(match[4].str()));
  persistIfAutoCommit(target.name());
  return "CREATE INDEX";
}

string Database::insert(const string& sql) {
  static const regex insertRegex(
      R"(^INSERT\s+INTO\s+([A-Za-z_][A-Za-z0-9_]*)\s+VALUES\s*\(([\s\S]*)\)$)",
      regex::icase);
  smatch match;
  if (!regex_match(sql, match, insertRegex)) throw runtime_error("syntax: INSERT INTO table VALUES (...)");
  auto& target = table(match[1].str());
  const auto message = target.insert(splitComma(match[2].str()));
  persistIfAutoCommit(target.name());
  return message;
}

string Database::select(const string& sql, bool explainOnly) {
  auto query = parseSelectQuery(sql, explainOnly);
  if (query.projection.empty() || query.projection.back().rfind("__table__", 0) != 0) {
    throw runtime_error("internal parser error: missing table");
  }
  const auto tableName = query.projection.back().substr(9);
  query.projection.pop_back();
  for (auto& column : query.projection) {
    if (column != "*") column = lower(column);
  }
  
  // If there are no joins, use the original single-table select
  if (query.joins.empty()) {
    return table(tableName).select(query);
  }
  
  // Handle JOIN queries
  return executeJoin(tableName, query);
}

string Database::update(const string& sql) {
  static const regex updateRegex(
      R"(^UPDATE\s+([A-Za-z_][A-Za-z0-9_]*)\s+SET\s+([\s\S]+?)(?:\s+WHERE\s+([\s\S]+))?$)",
      regex::icase);
  smatch match;
  if (!regex_match(sql, match, updateRegex)) {
    throw runtime_error("syntax: UPDATE table SET col = value [, ...] [WHERE condition]");
  }
  vector<pair<string, string>> assignments;
  for (const auto& part : splitComma(match[2].str())) {
    const auto pos = part.find('=');
    if (pos == string::npos) throw runtime_error("bad SET assignment: " + part);
    assignments.emplace_back(lower(trim(part.substr(0, pos))), trim(part.substr(pos + 1)));
  }
  auto& target = table(match[1].str());
  const auto message = target.update(assignments, parsePredicates(match[3].str()));
  persistIfAutoCommit(target.name());
  return message;
}

string Database::erase(const string& sql) {
  static const regex deleteRegex(
      R"(^DELETE\s+FROM\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+WHERE\s+([\s\S]+))?$)",
      regex::icase);
  smatch match;
  if (!regex_match(sql, match, deleteRegex)) {
    throw runtime_error("syntax: DELETE FROM table [WHERE condition]");
  }
  auto& target = table(match[1].str());
  const auto message = target.erase(parsePredicates(match[2].str()));
  persistIfAutoCommit(target.name());
  return message;
}

string Database::showTables() const {
  ostringstream out;
  out << "tables\n------\n";
  for (const auto& [name, _] : tables_) out << name << "\n";
  return out.str();
}

string Database::describe(const string& sql) const {
  stringstream ss(sql);
  string command, tableName;
  ss >> command >> tableName;
  if (tableName.empty()) throw runtime_error("syntax: DESCRIBE table");
  return table(tableName).describe();
}

string Database::vacuum(const string& sql) {
  stringstream ss(sql);
  string command, tableName;
  ss >> command >> tableName;
  if (tableName.empty()) throw runtime_error("syntax: VACUUM table");
  auto& target = table(tableName);
  const auto message = target.vacuum();
  persistIfAutoCommit(target.name());
  return message;
}

string Database::executeJoin(const string& leftTableName, const SelectQuery& query) const {
  const auto& leftTable = table(leftTableName);
  
  if (query.joins.empty()) {
    throw runtime_error("executeJoin called without joins");
  }
  
  // For simplicity, we'll only handle a single join for now
  // Multi-table joins can be added later
  const auto& join = query.joins[0];
  const auto& rightTable = table(join.rightTable);
  
  // Get column indexes
  int leftColIdx = -1;
  int rightColIdx = -1;
  
  for (size_t i = 0; i < leftTable.columns().size(); ++i) {
    if (leftTable.columns()[i].name == join.leftColumn) {
      leftColIdx = static_cast<int>(i);
      break;
    }
  }
  
  for (size_t i = 0; i < rightTable.columns().size(); ++i) {
    if (rightTable.columns()[i].name == join.rightColumn) {
      rightColIdx = static_cast<int>(i);
      break;
    }
  }
  
  if (leftColIdx < 0) {
    throw runtime_error("unknown join column in left table: " + join.leftColumn);
  }
  if (rightColIdx < 0) {
    throw runtime_error("unknown join column in right table: " + join.rightColumn);
  }
  
  vector<string> headers;
  if (query.projection.size() == 1 && trim(query.projection[0]) == "*") {
    for (const auto& col : leftTable.columns()) {
      headers.push_back(leftTable.name() + "." + col.name);
    }
    for (const auto& col : rightTable.columns()) {
      headers.push_back(rightTable.name() + "." + col.name);
    }
  } else {
    headers = query.projection;
  }
  
  // Join:
  vector<vector<string>> resultRows;
  const auto& leftRows = leftTable.rows();
  const auto& rightRows = rightTable.rows();
  
  ostringstream planDesc;
  planDesc << "PLAN: " << upper(joinTypeName(join.type)) << " JOIN between " 
           << leftTable.name() << " and " << rightTable.name() 
           << " ON " << leftTable.name() << "." << join.leftColumn 
           << " = " << rightTable.name() << "." << join.rightColumn;
  
  size_t leftRowsProcessed = 0;
  size_t rightRowsProcessed = 0;
  
  if (join.type == JoinType::Cross) {
    // Cross Join:
    for (size_t i = 0; i < leftRows.size(); ++i) {
      if (!leftRows[i].has_value()) continue;
      leftRowsProcessed++;
      
      for (size_t j = 0; j < rightRows.size(); ++j) {
        if (!rightRows[j].has_value()) continue;
        if (i == 0) rightRowsProcessed++;
        
        vector<string> row;
        const auto& leftRow = leftRows[i].value();
        const auto& rightRow = rightRows[j].value();
        
        if (query.projection.size() == 1 && trim(query.projection[0]) == "*") {
          row.insert(row.end(), leftRow.begin(), leftRow.end());
          row.insert(row.end(), rightRow.begin(), rightRow.end());
        } else {
          for (const auto& colSpec : query.projection) {
            auto dotPos = colSpec.find('.');
            if (dotPos != string::npos) {
              string tableName = lower(colSpec.substr(0, dotPos));
              string colName = lower(colSpec.substr(dotPos + 1));
              
              if (tableName == leftTable.name()) {
                for (size_t k = 0; k < leftTable.columns().size(); ++k) {
                  if (leftTable.columns()[k].name == colName) {
                    row.push_back(leftRow[k]);
                    break;
                  }
                }
              } else if (tableName == rightTable.name()) {
                for (size_t k = 0; k < rightTable.columns().size(); ++k) {
                  if (rightTable.columns()[k].name == colName) {
                    row.push_back(rightRow[k]);
                    break;
                  }
                }
              }
            }
          }
        }
        resultRows.push_back(row);
      }
    }
  } else {
    unordered_map<string, vector<size_t>> rightIndex;
    for (size_t j = 0; j < rightRows.size(); ++j) {
      if (!rightRows[j].has_value()) continue;
      const auto& key = rightRows[j].value()[static_cast<size_t>(rightColIdx)];
      rightIndex[key].push_back(j);
    }
    
    set<size_t> matchedRightRows;
    
    for (size_t i = 0; i < leftRows.size(); ++i) {
      if (!leftRows[i].has_value()) continue;
      leftRowsProcessed++;
      
      const auto& leftRow = leftRows[i].value();
      const auto& leftKey = leftRow[static_cast<size_t>(leftColIdx)];
      
      bool foundMatch = false;
      auto it = rightIndex.find(leftKey);
      
      if (it != rightIndex.end()) {
        for (size_t rightRowIdx : it->second) {
          foundMatch = true;
          matchedRightRows.insert(rightRowIdx);
          rightRowsProcessed++;
          
          const auto& rightRow = rightRows[rightRowIdx].value();
          
          vector<string> row;
          if (query.projection.size() == 1 && trim(query.projection[0]) == "*") {
            row.insert(row.end(), leftRow.begin(), leftRow.end());
            row.insert(row.end(), rightRow.begin(), rightRow.end());
          } else {
            for (const auto& colSpec : query.projection) {
              auto dotPos = colSpec.find('.');
              if (dotPos != string::npos) {
                string tableName = lower(colSpec.substr(0, dotPos));
                string colName = lower(colSpec.substr(dotPos + 1));
                
                if (tableName == leftTable.name()) {
                  for (size_t k = 0; k < leftTable.columns().size(); ++k) {
                    if (leftTable.columns()[k].name == colName) {
                      row.push_back(leftRow[k]);
                      break;
                    }
                  }
                } else if (tableName == rightTable.name()) {
                  for (size_t k = 0; k < rightTable.columns().size(); ++k) {
                    if (rightTable.columns()[k].name == colName) {
                      row.push_back(rightRow[k]);
                      break;
                    }
                  }
                }
              }
            }
          }
          resultRows.push_back(row);
        }
      }
      
      // Left JOin:
      if (!foundMatch && (join.type == JoinType::Left || join.type == JoinType::Full)) {
        vector<string> row;
        if (query.projection.size() == 1 && trim(query.projection[0]) == "*") {
          row.insert(row.end(), leftRow.begin(), leftRow.end());
          for (size_t k = 0; k < rightTable.columns().size(); ++k) {
            row.push_back("NULL");
          }
        } else {
          for (const auto& colSpec : query.projection) {
            auto dotPos = colSpec.find('.');
            if (dotPos != string::npos) {
              string tableName = lower(colSpec.substr(0, dotPos));
              string colName = lower(colSpec.substr(dotPos + 1));
              
              if (tableName == leftTable.name()) {
                for (size_t k = 0; k < leftTable.columns().size(); ++k) {
                  if (leftTable.columns()[k].name == colName) {
                    row.push_back(leftRow[k]);
                    break;
                  }
                }
              } else if (tableName == rightTable.name()) {
                row.push_back("NULL");
              }
            }
          }
        }
        resultRows.push_back(row);
      }
    }
    
    // Right Join:
    if (join.type == JoinType::Right || join.type == JoinType::Full) {
      for (size_t j = 0; j < rightRows.size(); ++j) {
        if (!rightRows[j].has_value()) continue;
        if (matchedRightRows.find(j) != matchedRightRows.end()) continue;
        
        const auto& rightRow = rightRows[j].value();
        vector<string> row;
        
        if (query.projection.size() == 1 && trim(query.projection[0]) == "*") {
          for (size_t k = 0; k < leftTable.columns().size(); ++k) {
            row.push_back("NULL");
          }
          row.insert(row.end(), rightRow.begin(), rightRow.end());
        } else {
          for (const auto& colSpec : query.projection) {
            auto dotPos = colSpec.find('.');
            if (dotPos != string::npos) {
              string tableName = lower(colSpec.substr(0, dotPos));
              string colName = lower(colSpec.substr(dotPos + 1));
              
              if (tableName == leftTable.name()) {
                row.push_back("NULL");
              } else if (tableName == rightTable.name()) {
                for (size_t k = 0; k < rightTable.columns().size(); ++k) {
                  if (rightTable.columns()[k].name == colName) {
                    row.push_back(rightRow[k]);
                    break;
                  }
                }
              }
            }
          }
        }
        resultRows.push_back(row);
      }
    }
  }
  
  planDesc << " | left_rows=" << leftRowsProcessed 
           << " | right_rows=" << rightRowsProcessed
           << " | rows_returned=" << resultRows.size();
  
  if (query.explainOnly) {
    return planDesc.str();
  }
  
  // Apply LIMIT if specified
  if (query.limit.has_value() && resultRows.size() > query.limit.value()) {
    resultRows.resize(query.limit.value());
  }
  
  return formatRows(headers, resultRows, planDesc.str());
}

}
