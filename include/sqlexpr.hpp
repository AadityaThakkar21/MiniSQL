#pragma once

#include "minidb.hpp"

#include <memory>
#include <string>
#include <vector>

namespace minidb {

// ---------------------------------------------------------------------------
// Tokens (lexer output)
// ---------------------------------------------------------------------------
enum class TokKind {
  Identifier, Number, String, Keyword,
  Plus, Minus, Star, Slash,
  Eq, Ne, Lt, Le, Gt, Ge,
  LParen, RParen, Comma, Dot, End
};

struct Token {
  TokKind kind;
  std::string text;  // normalized text (operators canonicalized, keywords upper)
  std::size_t pos;   // 0-based offset in the source, for error messages
};

// Turn raw SQL text into a token stream. Throws std::runtime_error on a bad
// character / unterminated literal, with the offending position.
std::vector<Token> lexSql(const std::string& input);

// ---------------------------------------------------------------------------
// Expression AST (parser output)
// ---------------------------------------------------------------------------
enum class ExprKind { IntLit, StrLit, NullLit, BoolLit, Column, Unary, Binary, Call };

struct Expr {
  ExprKind kind;
  long long intValue = 0;    // IntLit
  bool boolValue = false;    // BoolLit
  std::string text;          // StrLit value | Column name | operator | function name
  std::string table;         // Column qualifier (optional, lowercased)
  std::vector<std::shared_ptr<Expr>> args;  // Unary:1  Binary:2  Call:n
};
using ExprPtr = std::shared_ptr<Expr>;

// Parse a complete expression (e.g. a WHERE body) into an AST using a Pratt
// (precedence-climbing) parser. Throws on syntax error.
ExprPtr parseExpression(const std::string& input);

// Semantic pass: verify every column reference resolves and every function
// call is known with the right arity. Throws on the first problem found.
void validateExpr(const Expr& expr, const std::vector<Column>& columns);

// Pull the top-level AND-chain of simple "column <cmp> literal" comparisons out
// of an expression so the planner can still drive index selection. Anything
// non-sargable (OR, arithmetic, functions) is left to the evaluator.
std::vector<Predicate> extractSargablePredicates(const ExprPtr& expr);

// Evaluate the expression against a single row. Returns true only for SQL TRUE;
// NULL and FALSE both yield false (three-valued logic is applied internally).
bool exprIsTrue(const Expr& expr, const std::vector<Column>& columns,
                const std::vector<std::string>& row);

}  // namespace minidb
