#include "minidb.hpp"
#include "sqlexpr.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>

using namespace std;

namespace minidb {

void Index::clear() {
  btree.clear();
  hash.clear();
}

void Index::add(const string& key, size_t rowId) {
  if (kind == IndexKind::Hash) {
    hash[key].insert(rowId);
  } else {
    btree[key].insert(rowId);
  }
}

void Index::remove(const string& key, size_t rowId) {
  if (kind == IndexKind::Hash) {
    auto it = hash.find(key);
    if (it != hash.end()) {
      it->second.erase(rowId);
      if (it->second.empty()) hash.erase(it);
    }
  } else {
    auto it = btree.find(key);
    if (it != btree.end()) {
      it->second.erase(rowId);
      if (it->second.empty()) btree.erase(it);
    }
  }
}

set<size_t> Index::equality(const string& key) const {
  if (kind == IndexKind::Hash) {
    auto it = hash.find(key);
    return it != hash.end() ? it->second : set<size_t>{};
  }
  auto it = btree.find(key);
  return it != btree.end() ? it->second : set<size_t>{};
}

set<size_t> Index::range(const string& op, const string& key) const {
  // Only ordered (B-tree) indexes support range scans; the planner guards
  // this so hash indexes never reach here for <, <=, >, >=.
  set<size_t> result;
  for (const auto& [k, ids] : btree) {
    bool include = false;
    if (op == "<") include = k < key;
    else if (op == "<=") include = k <= key;
    else if (op == ">") include = k > key;
    else if (op == ">=") include = k >= key;
    if (include) result.insert(ids.begin(), ids.end());
  }
  return result;
}


string trim(string text) {
  const char* ws = " \t\n\r\f\v";
  const auto first = text.find_first_not_of(ws);
  if (first == string::npos) return "";
  const auto last = text.find_last_not_of(ws);
  return text.substr(first, last - first + 1);
}

string lower(string text) {
  transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return text;
}

string upper(string text) {
  transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return static_cast<char>(toupper(c)); });
  return text;
}

bool startsWithKeyword(const string& sql, const string& keyword) {
  const auto s = upper(trim(sql));
  const auto k = upper(keyword);
  if (s.size() < k.size()) return false;
  if (s.compare(0, k.size(), k) != 0) return false;
  if (s.size() == k.size()) return true;
  const char after = s[k.size()];
  return !(isalnum(static_cast<unsigned char>(after)) || after == '_');
}

// Find a keyword as a standalone, unquoted word (case-insensitive).
size_t findKeyword(const string& text, const string& keyword) {
  const auto up = upper(text);
  const auto kw = upper(keyword);
  if (kw.empty() || text.size() < kw.size()) return string::npos;
  char quote = 0;
  for (size_t i = 0; i + kw.size() <= text.size(); ++i) {
    const char c = text[i];
    if (quote) {
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      continue;
    }
    if (up.compare(i, kw.size(), kw) == 0) {
      const bool prevIdent =
          i > 0 && (isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '_');
      const size_t after = i + kw.size();
      const bool nextIdent =
          after < text.size() &&
          (isalnum(static_cast<unsigned char>(text[after])) || text[after] == '_');
      if (!prevIdent && !nextIdent) return i;
    }
  }
  return string::npos;
}

// Split on top-level commas, respecting nested parentheses and quotes.
vector<string> splitComma(const string& text) {
  vector<string> parts;
  if (trim(text).empty()) return parts;
  string current;
  int depth = 0;
  char quote = 0;
  for (const char c : text) {
    if (quote) {
      current += c;
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      current += c;
      continue;
    }
    if (c == '(') { ++depth; current += c; continue; }
    if (c == ')') { --depth; current += c; continue; }
    if (c == ',' && depth == 0) {
      parts.push_back(trim(current));
      current.clear();
      continue;
    }
    current += c;
  }
  parts.push_back(trim(current));
  return parts;
}

// Split a WHERE body on the AND keyword (unquoted, whole word).
vector<string> splitAnd(const string& text) {
  vector<string> parts;
  string rest = text;
  while (true) {
    const size_t p = findKeyword(rest, "AND");
    if (p == string::npos) {
      parts.push_back(trim(rest));
      break;
    }
    parts.push_back(trim(rest.substr(0, p)));
    rest = rest.substr(p + 3);
  }
  vector<string> out;
  for (auto& s : parts)
    if (!s.empty()) out.push_back(s);
  return out;
}

string unquote(string value) {
  value = trim(value);
  if (value.size() >= 2 &&
      ((value.front() == '\'' && value.back() == '\'') ||
       (value.front() == '"' && value.back() == '"'))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

bool isInteger(const string& value) {
  if (value.empty()) return false;
  size_t i = 0;
  if (value[0] == '+' || value[0] == '-') i = 1;
  if (i == value.size()) return false;
  for (; i < value.size(); ++i)
    if (!isdigit(static_cast<unsigned char>(value[i]))) return false;
  return true;
}


FieldType parseType(const string& text) {
  const auto u = upper(trim(text));
  if (u == "INT" || u == "INTEGER") return FieldType::Int;
  if (u == "TEXT" || u == "STRING" || u == "VARCHAR") return FieldType::Text;
  throw runtime_error("unknown column type: " + text);
}

string typeName(FieldType type) {
  return type == FieldType::Int ? "INT" : "TEXT";
}

IndexKind parseIndexKind(const string& text) {
  const auto u = upper(trim(text));
  if (u == "HASH") return IndexKind::Hash;
  return IndexKind::BTree;  // default + "BTREE"
}

string indexKindName(IndexKind kind) {
  return kind == IndexKind::Hash ? "HASH" : "BTREE";
}

string joinTypeName(JoinType type) {
  switch (type) {
    case JoinType::Inner: return "INNER";
    case JoinType::Left: return "LEFT";
    case JoinType::Right: return "RIGHT";
    case JoinType::Full: return "FULL";
    case JoinType::Cross: return "CROSS";
  }
  return "INNER";
}


string encode(const string& value) {
  string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c == '\\') out += "\\\\";
    else if (c == '\t') out += "\\t";
    else if (c == '\n') out += "\\n";
    else out += c;
  }
  return out;
}

string decode(const string& value) {
  string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\' && i + 1 < value.size()) {
      const char n = value[++i];
      if (n == '\\') out += '\\';
      else if (n == 't') out += '\t';
      else if (n == 'n') out += '\n';
      else out += n;
    } else {
      out += value[i];
    }
  }
  return out;
}

vector<string> splitTsv(const string& line) {
  vector<string> parts;
  string current;
  for (const char c : line) {
    if (c == '\t') {
      parts.push_back(current);
      current.clear();
    } else if (c != '\r') {
      current += c;
    }
  }
  parts.push_back(current);
  return parts;
}

// ---------------------------------------------------------------------------
// Statement splitting
// ---------------------------------------------------------------------------

vector<string> splitStatements(const string& input) {
  vector<string> out;
  string current;
  char quote = 0;
  for (size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];
    if (quote) {
      current += c;
      if (c == quote) quote = 0;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      current += c;
      continue;
    }
    if (c == '-' && i + 1 < input.size() && input[i + 1] == '-') {
      while (i < input.size() && input[i] != '\n') ++i;  // skip to end of line
      continue;
    }
    if (c == ';') {
      if (!trim(current).empty()) out.push_back(trim(current));
      current.clear();
      continue;
    }
    current += c;
  }
  if (!trim(current).empty()) out.push_back(trim(current));
  return out;
}

vector<Predicate> parsePredicates(const string& text) {
  vector<Predicate> predicates;
  if (trim(text).empty()) return predicates;
  static const char* operators[] = {">=", "<=", "!=", "=", "<", ">"};
  for (const auto& clause : splitAnd(text)) {
    size_t pos = string::npos;
    string opFound;
    for (const auto* op : operators) {
      const size_t f = clause.find(op);
      if (f != string::npos) {
        pos = f;
        opFound = op;
        break;
      }
    }
    if (pos == string::npos) throw runtime_error("bad predicate: " + clause);
    Predicate p;
    p.column = lower(trim(clause.substr(0, pos)));
    p.op = opFound;
    p.value = unquote(trim(clause.substr(pos + opFound.size())));
    predicates.push_back(p);
  }
  return predicates;
}

optional<Aggregate> parseAggregate(const vector<string>& projection) {
  if (projection.size() != 1) return nullopt;
  static const regex re(R"(^(COUNT|AVG|MIN|MAX|SUM)\s*\(\s*(.*?)\s*\)$)", regex::icase);
  smatch m;
  const string p = trim(projection[0]);
  if (!regex_match(p, m, re)) return nullopt;
  Aggregate a;
  a.function = upper(m[1].str());
  const string col = trim(m[2].str());
  a.column = (col == "*") ? "*" : lower(col);
  return a;
}

// ---------------------------------------------------------------------------
// Result formatting
// ---------------------------------------------------------------------------

string formatRows(const vector<string>& headers,
                  const vector<vector<string>>& rows,
                  const string& plan) {
  ostringstream out;
  if (!plan.empty()) out << plan << "\n";
  if (headers.empty()) return out.str();

  vector<size_t> widths(headers.size());
  for (size_t i = 0; i < headers.size(); ++i) widths[i] = headers[i].size();
  for (const auto& row : rows)
    for (size_t i = 0; i < row.size() && i < widths.size(); ++i)
      widths[i] = max(widths[i], row[i].size());

  for (size_t i = 0; i < headers.size(); ++i) {
    if (i) out << " | ";
    out << left << setw(static_cast<int>(widths[i])) << headers[i];
  }
  out << "\n";
  for (size_t i = 0; i < headers.size(); ++i) {
    if (i) out << "-+-";
    out << string(widths[i], '-');
  }
  out << "\n";
  for (const auto& row : rows) {
    for (size_t i = 0; i < headers.size(); ++i) {
      if (i) out << " | ";
      const string cell = i < row.size() ? row[i] : "";
      out << left << setw(static_cast<int>(widths[i])) << cell;
    }
    out << "\n";
  }
  out << "(" << rows.size() << " row" << (rows.size() == 1 ? "" : "s") << ")";
  return out.str();
}

// ---------------------------------------------------------------------------
// SELECT parser
// ---------------------------------------------------------------------------

namespace {
size_t earliestJoinKeyword(const string& text) {
  size_t best = string::npos;
  for (const char* k : {"INNER", "LEFT", "RIGHT", "FULL", "CROSS", "JOIN"}) {
    const size_t p = findKeyword(text, k);
    if (p < best) best = p;
  }
  return best;
}

string joinTokenSlice(const vector<Token>& toks, size_t begin, size_t end) {
  string out;
  for (size_t i = begin; i < end; ++i) {
    if (i > begin &&
        (toks[i].kind == TokKind::Identifier || toks[i].kind == TokKind::Number ||
         toks[i].kind == TokKind::String || toks[i].kind == TokKind::Keyword ||
         toks[i].kind == TokKind::Plus || toks[i].kind == TokKind::Minus ||
         toks[i].kind == TokKind::Star || toks[i].kind == TokKind::Slash ||
         toks[i].kind == TokKind::Eq || toks[i].kind == TokKind::Ne ||
         toks[i].kind == TokKind::Lt || toks[i].kind == TokKind::Le ||
         toks[i].kind == TokKind::Gt || toks[i].kind == TokKind::Ge ||
         toks[i].kind == TokKind::LParen || toks[i].kind == TokKind::RParen ||
         toks[i].kind == TokKind::Comma || toks[i].kind == TokKind::Dot)) {
      out += ' ';
    }
    out += toks[i].text;
  }
  return trim(out);
}

pair<string, string> splitAlias(const string& text) {
  const string trimmed = trim(text);
  if (trimmed.empty()) return {"", ""};

  const auto toks = lexSql(trimmed);
  int depth = 0;
  size_t asIndex = string::npos;
  for (size_t i = 0; i < toks.size(); ++i) {
    if (toks[i].kind == TokKind::LParen) ++depth;
    else if (toks[i].kind == TokKind::RParen && depth > 0) --depth;
    else if (depth == 0 && toks[i].kind == TokKind::Keyword && toks[i].text == "AS") {
      asIndex = i;
      break;
    }
  }
  if (asIndex == string::npos || asIndex == 0 || asIndex + 1 >= toks.size()) {
    return {trimmed, ""};
  }

  const string expr = joinTokenSlice(toks, 0, asIndex);
  const string aliasText = joinTokenSlice(toks, asIndex + 1, toks.size() - 1);
  if (expr.empty() || aliasText.empty()) return {trimmed, ""};
  return {expr, aliasText};
}

void splitQualified(const string& operand, string& tableOut, string& columnOut) {
  const size_t dot = operand.find('.');
  if (dot == string::npos) {
    tableOut.clear();
    columnOut = lower(trim(operand));
  } else {
    tableOut = lower(trim(operand.substr(0, dot)));
    columnOut = lower(trim(operand.substr(dot + 1)));
  }
}
}  // namespace

SelectQuery parseSelectQuery(const string& sql, bool explainOnly) {
  SelectQuery query;
  query.explainOnly = explainOnly;

  string s = trim(sql);

  // Strip leading EXPLAIN [ANALYZE].
  if (upper(s).rfind("EXPLAIN", 0) == 0) {
    query.explainOnly = true;
    s = trim(s.substr(7));
    if (upper(s).rfind("ANALYZE", 0) == 0) s = trim(s.substr(7));
  }

  if (upper(s).rfind("SELECT", 0) != 0) throw runtime_error("expected SELECT");
  s = trim(s.substr(6));

  const size_t fromPos = findKeyword(s, "FROM");
  if (fromPos == string::npos) throw runtime_error("SELECT requires FROM");

  const string projText = trim(s.substr(0, fromPos));
  const string afterFrom = trim(s.substr(fromPos + 4));

  const size_t wherePos = findKeyword(afterFrom, "WHERE");
  const size_t groupPos = findKeyword(afterFrom, "GROUP");
  const size_t orderPos = findKeyword(afterFrom, "ORDER");
  const size_t limitPos = findKeyword(afterFrom, "LIMIT");

  const size_t clauseStart = min({wherePos, groupPos, orderPos, limitPos});
  const string fromJoins =
      clauseStart == string::npos ? afterFrom : trim(afterFrom.substr(0, clauseStart));

  // Table name + optional JOIN.
  const size_t jp = earliestJoinKeyword(fromJoins);
  string tableName = trim(jp == string::npos ? fromJoins : fromJoins.substr(0, jp));
  {
    stringstream ss(tableName);
    string tok;
    ss >> tok;
    tableName = tok;
  }

  if (jp != string::npos) {
    string joinText = trim(fromJoins.substr(jp));
    JoinClause jc;
    const string ju = upper(joinText);
    size_t consume = 0;
    if (ju.rfind("INNER", 0) == 0) { jc.type = JoinType::Inner; consume = 5; }
    else if (ju.rfind("LEFT", 0) == 0) { jc.type = JoinType::Left; consume = 4; }
    else if (ju.rfind("RIGHT", 0) == 0) { jc.type = JoinType::Right; consume = 5; }
    else if (ju.rfind("FULL", 0) == 0) { jc.type = JoinType::Full; consume = 4; }
    else if (ju.rfind("CROSS", 0) == 0) { jc.type = JoinType::Cross; consume = 5; }
    else { jc.type = JoinType::Inner; consume = 0; }  // bare JOIN
    joinText = trim(joinText.substr(consume));
    if (upper(joinText).rfind("OUTER", 0) == 0) joinText = trim(joinText.substr(5));
    if (upper(joinText).rfind("JOIN", 0) == 0) joinText = trim(joinText.substr(4));

    string rightTable;
    {
      stringstream ss(joinText);
      ss >> rightTable;
    }
    jc.rightTable = lower(rightTable);
    const size_t rtPos = joinText.find(rightTable);
    joinText = trim(joinText.substr(rtPos + rightTable.size()));

    if (jc.type != JoinType::Cross) {
      const size_t onPos = findKeyword(joinText, "ON");
      if (onPos == string::npos) throw runtime_error("JOIN requires ON");
      const string cond = trim(joinText.substr(onPos + 2));
      const size_t eq = cond.find('=');
      if (eq == string::npos) throw runtime_error("JOIN ON requires =");
      string lt, lc, rt, rc;
      splitQualified(trim(cond.substr(0, eq)), lt, lc);
      splitQualified(trim(cond.substr(eq + 1)), rt, rc);
      if (rt == jc.rightTable) { jc.leftColumn = lc; jc.rightColumn = rc; }
      else if (lt == jc.rightTable) { jc.leftColumn = rc; jc.rightColumn = lc; }
      else { jc.leftColumn = lc; jc.rightColumn = rc; }
    }
    query.joins.push_back(jc);
  }

  // WHERE
  if (wherePos != string::npos) {
    size_t end = afterFrom.size();
    for (const size_t p : {groupPos, orderPos, limitPos})
      if (p != string::npos && p > wherePos) end = min(end, p);
    const string whereText = trim(afterFrom.substr(wherePos + 5, end - (wherePos + 5)));
    if (!whereText.empty()) {
      query.whereExpr = parseExpression(whereText);
      query.predicates = extractSargablePredicates(query.whereExpr);
    }
  }

  // ORDER BY
  if (orderPos != string::npos) {
    size_t end = afterFrom.size();
    if (limitPos != string::npos && limitPos > orderPos) end = limitPos;
    string ob = trim(afterFrom.substr(orderPos + 5, end - (orderPos + 5)));
    if (upper(ob).rfind("BY", 0) == 0) ob = trim(ob.substr(2));
    stringstream ss(ob);
    string col, dir;
    ss >> col >> dir;
    SortSpec sort;
    sort.column = lower(col);
    sort.direction = upper(dir) == "DESC" ? SortDirection::Desc : SortDirection::Asc;
    query.orderBy = sort;
  }

  // LIMIT
  if (limitPos != string::npos) {
    const string lt = trim(afterFrom.substr(limitPos + 5));
    stringstream ss(lt);
    long long n = 0;
    if ((ss >> n) && n >= 0) query.limit = static_cast<size_t>(n);
  }

  // Projection
  vector<string> projection;
  vector<string> aliases;
  for (const auto& raw : splitComma(projText)) {
    const auto [expr, alias] = splitAlias(raw);
    projection.push_back(expr);
    aliases.push_back(alias);
  }
  if (projection.empty()) projection.push_back("*");

  query.aggregate = parseAggregate(projection);
  query.projection = projection;
  query.aliases = aliases;
  query.projection.push_back("__table__" + tableName);  // sentinel for Database::select
  return query;
}

}  // namespace minidb
