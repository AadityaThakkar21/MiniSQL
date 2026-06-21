#include "sqlexpr.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

using namespace std;

namespace minidb {
vector<Token> lexSql(const string& input) {
  vector<Token> tokens;
  const size_t n = input.size();
  size_t i = 0;

  auto identStart = [](char c) { return isalpha(static_cast<unsigned char>(c)) || c == '_'; };
  auto identChar = [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; };
  auto isKeyword = [](const string& word) {
    const string up = upper(word);
    static const vector<string> kws = {
      "SELECT", "FROM", "WHERE", "GROUP", "ORDER", "LIMIT", "JOIN",
      "INNER", "LEFT", "RIGHT", "FULL", "OUTER", "CROSS", "ON",
      "AS", "BY", "ASC", "DESC", "USING", "CREATE", "TABLE",
      "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE",
      "BEGIN", "COMMIT", "ROLLBACK", "SHOW", "DESCRIBE", "VACUUM",
      "DROP", "INDEX", "IF", "EXISTS", "EXPLAIN", "ANALYZE",
      "AND", "OR", "NOT", "NULL", "TRUE", "FALSE"
    };
    return find(kws.begin(), kws.end(), up) != kws.end();
  };

  while (i < n) {
    const char c = input[i];
    if (isspace(static_cast<unsigned char>(c))) { ++i; continue; }

    // Line comment: -- to end of line.
    if (c == '-' && i + 1 < n && input[i + 1] == '-') {
      while (i < n && input[i] != '\n') ++i;
      continue;
    }

    const size_t start = i;

    if (identStart(c)) {
      while (i < n && identChar(input[i])) ++i;
      const string word = input.substr(start, i - start);
      if (isKeyword(word)) {
        tokens.push_back({TokKind::Keyword, upper(word), start});
      } else {
        tokens.push_back({TokKind::Identifier, word, start});
      }
      continue;
    }

    if (c == '"') {  // delimited identifier
      ++i;
      string s;
      while (i < n && input[i] != '"') s += input[i++];
      if (i >= n) throw runtime_error("unterminated quoted identifier at position " + to_string(start));
      ++i;
      tokens.push_back({TokKind::Identifier, s, start});
      continue;
    }

    if (c == '\'') {  // string literal ('' is an escaped quote)
      ++i;
      string s;
      while (i < n) {
        if (input[i] == '\'') {
          if (i + 1 < n && input[i + 1] == '\'') { s += '\''; i += 2; continue; }
          break;
        }
        s += input[i++];
      }
      if (i >= n) throw runtime_error("unterminated string literal at position " + to_string(start));
      ++i;
      tokens.push_back({TokKind::String, s, start});
      continue;
    }

    if (isdigit(static_cast<unsigned char>(c))) {
      while (i < n && (isdigit(static_cast<unsigned char>(input[i])) || input[i] == '.')) ++i;
      tokens.push_back({TokKind::Number, input.substr(start, i - start), start});
      continue;
    }

    auto two = [&](char a, char b) { return i + 1 < n && input[i] == a && input[i + 1] == b; };
    if (two('=', '=')) { tokens.push_back({TokKind::Eq, "=", start}); i += 2; continue; }
    if (two('!', '=')) { tokens.push_back({TokKind::Ne, "!=", start}); i += 2; continue; }
    if (two('<', '>')) { tokens.push_back({TokKind::Ne, "!=", start}); i += 2; continue; }
    if (two('<', '=')) { tokens.push_back({TokKind::Le, "<=", start}); i += 2; continue; }
    if (two('>', '=')) { tokens.push_back({TokKind::Ge, ">=", start}); i += 2; continue; }

    switch (c) {
      case '=': tokens.push_back({TokKind::Eq, "=", start}); break;
      case '<': tokens.push_back({TokKind::Lt, "<", start}); break;
      case '>': tokens.push_back({TokKind::Gt, ">", start}); break;
      case '+': tokens.push_back({TokKind::Plus, "+", start}); break;
      case '-': tokens.push_back({TokKind::Minus, "-", start}); break;
      case '*': tokens.push_back({TokKind::Star, "*", start}); break;
      case '/': tokens.push_back({TokKind::Slash, "/", start}); break;
      case '(': tokens.push_back({TokKind::LParen, "(", start}); break;
      case ')': tokens.push_back({TokKind::RParen, ")", start}); break;
      case ',': tokens.push_back({TokKind::Comma, ",", start}); break;
      case '.': tokens.push_back({TokKind::Dot, ".", start}); break;
      default:
        throw runtime_error("unexpected character '" + string(1, c) + "' at position " + to_string(start));
    }
    ++i;
  }

  tokens.push_back({TokKind::End, "", n});
  return tokens;
}

// ===========================================================================
// Pratt parser
// ===========================================================================
namespace {

// Binding powers. Higher binds tighter. Comparisons are non-associative in
// spirit but parsed left-associative here.
constexpr int kBpOr = 1;
constexpr int kBpAnd = 2;
constexpr int kBpCompare = 3;
constexpr int kBpAdd = 4;
constexpr int kBpMul = 5;
constexpr int kBpUnaryMinus = 6;  // binds tighter than * and /

int leftBindingPower(const Token& t) {
  if (t.kind == TokKind::Keyword) {
    if (t.text == "OR") return kBpOr;
    if (t.text == "AND") return kBpAnd;
    return 0;
  }
  switch (t.kind) {
    case TokKind::Eq: case TokKind::Ne:
    case TokKind::Lt: case TokKind::Le:
    case TokKind::Gt: case TokKind::Ge: return kBpCompare;
    case TokKind::Plus: case TokKind::Minus: return kBpAdd;
    case TokKind::Star: case TokKind::Slash: return kBpMul;
    default: return 0;
  }
}

string binaryOpText(const Token& t) {
  if (t.kind == TokKind::Keyword) return t.text;  // AND / OR
  return t.text;  // operators are already canonicalized by the lexer
}

ExprPtr makeExpr(ExprKind kind) {
  auto e = make_shared<Expr>();
  e->kind = kind;
  return e;
}

struct Parser {
  const vector<Token>& toks;
  size_t pos = 0;

  explicit Parser(const vector<Token>& t) : toks(t) {}

  const Token& peek() const { return toks[pos]; }
  const Token& advance() { return toks[pos++]; }

  void expect(TokKind kind, const char* what) {
    if (peek().kind != kind) {
      throw runtime_error(string("expected ") + what + " but found '" + peek().text +
                          "' at position " + to_string(peek().pos));
    }
    advance();
  }

  ExprPtr parse() {
    ExprPtr e = parseExpr(0);
    if (peek().kind != TokKind::End) {
      throw runtime_error("unexpected trailing token '" + peek().text + "' at position " +
                          to_string(peek().pos));
    }
    return e;
  }

  ExprPtr parseExpr(int minBp) {
    ExprPtr left = parsePrefix();
    while (true) {
      const int lbp = leftBindingPower(peek());
      if (lbp == 0 || lbp <= minBp) break;
      const Token op = advance();
      ExprPtr right = parseExpr(lbp);  // left-associative
      auto node = makeExpr(ExprKind::Binary);
      node->text = binaryOpText(op);
      node->args = {left, right};
      left = node;
    }
    return left;
  }

  ExprPtr parsePrefix() {
    const Token& t = peek();

    if (t.kind == TokKind::Keyword && t.text == "NOT") {
      advance();
      auto node = makeExpr(ExprKind::Unary);
      node->text = "NOT";
      node->args = {parseExpr(kBpAnd)};  // NOT binds looser than comparison, tighter than AND
      return node;
    }
    if (t.kind == TokKind::Minus) {
      advance();
      auto node = makeExpr(ExprKind::Unary);
      node->text = "-";
      node->args = {parseExpr(kBpUnaryMinus)};
      return node;
    }
    if (t.kind == TokKind::Number) {
      advance();
      if (t.text.find('.') != string::npos) {
        throw runtime_error("non-integer literal '" + t.text + "' not supported");
      }
      auto node = makeExpr(ExprKind::IntLit);
      node->intValue = stoll(t.text);
      return node;
    }
    if (t.kind == TokKind::String) {
      advance();
      auto node = makeExpr(ExprKind::StrLit);
      node->text = t.text;
      return node;
    }
    if (t.kind == TokKind::Keyword && t.text == "NULL") {
      advance();
      return makeExpr(ExprKind::NullLit);
    }
    if (t.kind == TokKind::Keyword && (t.text == "TRUE" || t.text == "FALSE")) {
      advance();
      auto node = makeExpr(ExprKind::BoolLit);
      node->boolValue = (t.text == "TRUE");
      return node;
    }
    if (t.kind == TokKind::LParen) {
      advance();
      ExprPtr e = parseExpr(0);
      expect(TokKind::RParen, "')'");
      return e;
    }
    if (t.kind == TokKind::Identifier) {
      advance();
      // Function call: name '(' args ')'
      if (peek().kind == TokKind::LParen) {
        advance();
        auto node = makeExpr(ExprKind::Call);
        node->text = upper(t.text);
        if (peek().kind != TokKind::RParen) {
          node->args.push_back(parseExpr(0));
          while (peek().kind == TokKind::Comma) {
            advance();
            node->args.push_back(parseExpr(0));
          }
        }
        expect(TokKind::RParen, "')'");
        return node;
      }
      // Column, optionally qualified: table.column
      string qualifier;
      string column = t.text;
      if (peek().kind == TokKind::Dot) {
        advance();
        if (peek().kind != TokKind::Identifier) {
          throw runtime_error("expected column name after '.' at position " + to_string(peek().pos));
        }
        qualifier = column;
        column = advance().text;
      }
      auto node = makeExpr(ExprKind::Column);
      node->table = lower(qualifier);
      node->text = lower(column);
      return node;
    }

    throw runtime_error("unexpected token '" + t.text + "' at position " + to_string(t.pos));
  }
};

}  // namespace

ExprPtr parseExpression(const string& input) {
  const auto tokens = lexSql(input);
  Parser parser(tokens);
  return parser.parse();
}

// ===========================================================================
// Semantic validation
// ===========================================================================
namespace {

bool columnExists(const string& name, const vector<Column>& columns) {
  for (const auto& c : columns)
    if (c.name == name) return true;
  return false;
}

void checkFunction(const Expr& e) {
  const string& fn = e.text;
  const size_t argc = e.args.size();
  if (fn == "UPPER" || fn == "LOWER" || fn == "LENGTH" || fn == "ABS") {
    if (argc != 1) throw runtime_error(fn + "() takes exactly 1 argument");
  } else if (fn == "COALESCE") {
    if (argc < 1) throw runtime_error("COALESCE() takes at least 1 argument");
  } else {
    throw runtime_error("unknown function: " + fn);
  }
}

}  // namespace

void validateExpr(const Expr& expr, const vector<Column>& columns) {
  if (expr.kind == ExprKind::Column) {
    if (!columnExists(expr.text, columns)) {
      throw runtime_error("unknown column in WHERE: " +
                          (expr.table.empty() ? expr.text : expr.table + "." + expr.text));
    }
  } else if (expr.kind == ExprKind::Call) {
    checkFunction(expr);
  }
  for (const auto& child : expr.args) validateExpr(*child, columns);
}

// ===========================================================================
// Sargable predicate extraction (for index planning)
// ===========================================================================
namespace {

void collectConjuncts(const ExprPtr& e, vector<const Expr*>& out) {
  if (e->kind == ExprKind::Binary && e->text == "AND") {
    collectConjuncts(e->args[0], out);
    collectConjuncts(e->args[1], out);
  } else {
    out.push_back(e.get());
  }
}

string flipComparison(const string& op) {
  if (op == "<") return ">";
  if (op == "<=") return ">=";
  if (op == ">") return "<";
  if (op == ">=") return "<=";
  return op;  // = and != are symmetric
}

bool isComparison(const string& op) {
  return op == "=" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

}  // namespace

vector<Predicate> extractSargablePredicates(const ExprPtr& expr) {
  vector<Predicate> predicates;
  if (!expr) return predicates;

  vector<const Expr*> conjuncts;
  collectConjuncts(expr, conjuncts);

  for (const Expr* c : conjuncts) {
    if (c->kind != ExprKind::Binary || !isComparison(c->text)) continue;
    const Expr* left = c->args[0].get();
    const Expr* right = c->args[1].get();

    auto isCol = [](const Expr* e) { return e->kind == ExprKind::Column; };
    auto isLit = [](const Expr* e) { return e->kind == ExprKind::IntLit || e->kind == ExprKind::StrLit; };

    const Expr* col = nullptr;
    const Expr* lit = nullptr;
    string op = c->text;
    if (isCol(left) && isLit(right)) { col = left; lit = right; }
    else if (isLit(left) && isCol(right)) { col = right; lit = left; op = flipComparison(op); }
    else continue;

    Predicate p;
    p.column = col->table.empty() ? col->text : col->table + "." + col->text;
    p.op = op;
    p.value = (lit->kind == ExprKind::IntLit) ? to_string(lit->intValue) : lit->text;
    predicates.push_back(p);
  }
  return predicates;
}

// ===========================================================================
// Evaluator (three-valued logic)
// ===========================================================================
namespace {

struct Val {
  enum class T { Int, Str, Null, Bool } t;
  long long i = 0;
  string s;
  bool b = false;
};

Val vInt(long long n) { return {Val::T::Int, n, "", false}; }
Val vStr(string s) { return {Val::T::Str, 0, move(s), false}; }
Val vBool(bool b) { return {Val::T::Bool, 0, "", b}; }
Val vNull() { return {Val::T::Null, 0, "", false}; }

long long asInt(const Val& v) {
  if (v.t == Val::T::Int) return v.i;
  if (v.t == Val::T::Bool) return v.b ? 1 : 0;
  throw runtime_error("expected an integer value in expression");
}

// nullopt means UNKNOWN (NULL) in three-valued logic.
bool toBool(const Val& v, bool& out) {
  switch (v.t) {
    case Val::T::Null: return false;  // unknown
    case Val::T::Bool: out = v.b; return true;
    case Val::T::Int: out = (v.i != 0); return true;
    case Val::T::Str: throw runtime_error("non-boolean string used in a boolean context");
  }
  return false;
}

Val evalNode(const Expr& e, const vector<Column>& columns, const vector<string>& row);

Val evalLogical(const string& op, const Expr& e, const vector<Column>& columns,
                const vector<string>& row) {
  const Val a = evalNode(*e.args[0], columns, row);
  bool ab = false;
  const bool aKnown = toBool(a, ab);

  if (op == "AND") {
    if (aKnown && !ab) return vBool(false);  // FALSE dominates
    const Val b = evalNode(*e.args[1], columns, row);
    bool bb = false;
    const bool bKnown = toBool(b, bb);
    if (bKnown && !bb) return vBool(false);
    if (!aKnown || !bKnown) return vNull();
    return vBool(ab && bb);
  }
  // OR
  if (aKnown && ab) return vBool(true);  // TRUE dominates
  const Val b = evalNode(*e.args[1], columns, row);
  bool bb = false;
  const bool bKnown = toBool(b, bb);
  if (bKnown && bb) return vBool(true);
  if (!aKnown || !bKnown) return vNull();
  return vBool(ab || bb);
}

bool compareVals(const string& op, const Val& a, const Val& b) {
  if (a.t == Val::T::Int && b.t == Val::T::Int) {
    if (op == "=") return a.i == b.i;
    if (op == "!=") return a.i != b.i;
    if (op == "<") return a.i < b.i;
    if (op == "<=") return a.i <= b.i;
    if (op == ">") return a.i > b.i;
    return a.i >= b.i;
  }
  if (a.t == Val::T::Str && b.t == Val::T::Str) {
    if (op == "=") return a.s == b.s;
    if (op == "!=") return a.s != b.s;
    if (op == "<") return a.s < b.s;
    if (op == "<=") return a.s <= b.s;
    if (op == ">") return a.s > b.s;
    return a.s >= b.s;
  }
  throw runtime_error("type mismatch in comparison");
}

Val evalCall(const Expr& e, const vector<Column>& columns, const vector<string>& row) {
  const string& fn = e.text;
  vector<Val> args;
  args.reserve(e.args.size());
  for (const auto& a : e.args) args.push_back(evalNode(*a, columns, row));

  if (fn == "COALESCE") {
    for (const auto& v : args)
      if (v.t != Val::T::Null) return v;
    return vNull();
  }
  if (args[0].t == Val::T::Null) return vNull();
  if (fn == "UPPER") return vStr(upper(args[0].s));
  if (fn == "LOWER") return vStr(lower(args[0].s));
  if (fn == "LENGTH") return vInt(static_cast<long long>(args[0].s.size()));
  if (fn == "ABS") { long long n = asInt(args[0]); return vInt(n < 0 ? -n : n); }
  throw runtime_error("unknown function: " + fn);
}

Val evalNode(const Expr& e, const vector<Column>& columns, const vector<string>& row) {
  switch (e.kind) {
    case ExprKind::IntLit: return vInt(e.intValue);
    case ExprKind::StrLit: return vStr(e.text);
    case ExprKind::NullLit: return vNull();
    case ExprKind::BoolLit: return vBool(e.boolValue);
    case ExprKind::Column: {
      int idx = -1;
      for (size_t i = 0; i < columns.size(); ++i)
        if (columns[i].name == e.text) { idx = static_cast<int>(i); break; }
      if (idx < 0) {
        throw runtime_error("unknown column in WHERE: " +
                            (e.table.empty() ? e.text : e.table + "." + e.text));
      }
      const string& raw = row[static_cast<size_t>(idx)];
      if (columns[static_cast<size_t>(idx)].type == FieldType::Int) {
        if (raw.empty()) return vNull();
        return vInt(stoll(raw));
      }
      return vStr(raw);
    }
    case ExprKind::Unary: {
      if (e.text == "NOT") {
        const Val v = evalNode(*e.args[0], columns, row);
        bool tb = false;
        if (!toBool(v, tb)) return vNull();
        return vBool(!tb);
      }
      // unary minus
      const Val v = evalNode(*e.args[0], columns, row);
      if (v.t == Val::T::Null) return vNull();
      return vInt(-asInt(v));
    }
    case ExprKind::Binary: {
      const string& op = e.text;
      if (op == "AND" || op == "OR") return evalLogical(op, e, columns, row);

      const Val a = evalNode(*e.args[0], columns, row);
      const Val b = evalNode(*e.args[1], columns, row);

      if (op == "+" || op == "-" || op == "*" || op == "/") {
        if (a.t == Val::T::Null || b.t == Val::T::Null) return vNull();
        const long long x = asInt(a);
        const long long y = asInt(b);
        if (op == "+") return vInt(x + y);
        if (op == "-") return vInt(x - y);
        if (op == "*") return vInt(x * y);
        if (y == 0) throw runtime_error("division by zero");
        return vInt(x / y);
      }
      // comparison: NULL on either side -> UNKNOWN
      if (a.t == Val::T::Null || b.t == Val::T::Null) return vNull();
      return vBool(compareVals(op, a, b));
    }
    case ExprKind::Call: return evalCall(e, columns, row);
  }
  throw runtime_error("internal: unhandled expression node");
}

}  // namespace

bool exprIsTrue(const Expr& expr, const vector<Column>& columns, const vector<string>& row) {
  const Val v = evalNode(expr, columns, row);
  switch (v.t) {
    case Val::T::Bool: return v.b;
    case Val::T::Int: return v.i != 0;
    case Val::T::Null: return false;
    case Val::T::Str:
      throw runtime_error("WHERE clause did not evaluate to a boolean");
  }
  return false;
}

}  // namespace minidb
