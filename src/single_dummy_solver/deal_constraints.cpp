#include "deal_constraints.hpp"

#include <cctype>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace single_dummy_solver
{
namespace
{

enum class TokenKind
{
  end,
  integer,
  identifier,
  eq,
  ne,
  ge,
  le,
  gt,
  lt,
  lparen,
  rparen,
  lbracket,
  rbracket,
  dot,
  and_kw,
  or_kw,
  not_kw,
};

struct Token
{
  TokenKind kind = TokenKind::end;
  int integer = 0;
  std::string text;
};

class Lexer
{
public:
  explicit Lexer(std::string_view source) : source_(source) {}

  auto next() -> Token
  {
    skip_space();
    if (pos_ >= source_.size())
    {
      return Token{TokenKind::end};
    }

    const char ch = source_[pos_];
    if (std::isdigit(static_cast<unsigned char>(ch)))
    {
      return read_integer();
    }
    if (ch == '(')
    {
      ++pos_;
      return Token{TokenKind::lparen};
    }
    if (ch == ')')
    {
      ++pos_;
      return Token{TokenKind::rparen};
    }
    if (ch == '[')
    {
      ++pos_;
      return Token{TokenKind::lbracket};
    }
    if (ch == ']')
    {
      ++pos_;
      return Token{TokenKind::rbracket};
    }
    if (ch == '.')
    {
      ++pos_;
      return Token{TokenKind::dot};
    }
    if (ch == '=' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '=')
    {
      pos_ += 2;
      return Token{TokenKind::eq};
    }
    if (ch == '!' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '=')
    {
      pos_ += 2;
      return Token{TokenKind::ne};
    }
    if (ch == '>' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '=')
    {
      pos_ += 2;
      return Token{TokenKind::ge};
    }
    if (ch == '<' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '=')
    {
      pos_ += 2;
      return Token{TokenKind::le};
    }
    if (ch == '>')
    {
      ++pos_;
      return Token{TokenKind::gt};
    }
    if (ch == '<')
    {
      ++pos_;
      return Token{TokenKind::lt};
    }
    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_')
    {
      return read_identifier();
    }
    throw std::invalid_argument("invalid character in deal constraint");
  }

private:
  auto skip_space() -> void
  {
    while (pos_ < source_.size()
           && std::isspace(static_cast<unsigned char>(source_[pos_])))
    {
      ++pos_;
    }
  }

  auto read_integer() -> Token
  {
    const std::size_t start = pos_;
    while (pos_ < source_.size()
           && std::isdigit(static_cast<unsigned char>(source_[pos_])))
    {
      ++pos_;
    }
    Token token{TokenKind::integer};
    token.integer = std::stoi(std::string(source_.substr(start, pos_ - start)));
    return token;
  }

  auto read_identifier() -> Token
  {
    const std::size_t start = pos_;
    while (pos_ < source_.size()
           && (std::isalnum(static_cast<unsigned char>(source_[pos_]))
               || source_[pos_] == '_'))
    {
      ++pos_;
    }
    Token token{TokenKind::identifier};
    token.text = std::string(source_.substr(start, pos_ - start));
    if (token.text == "and")
    {
      token.kind = TokenKind::and_kw;
    }
    else if (token.text == "or")
    {
      token.kind = TokenKind::or_kw;
    }
    else if (token.text == "not")
    {
      token.kind = TokenKind::not_kw;
    }
    else
    {
      token.kind = TokenKind::identifier;
    }
    return token;
  }

  std::string_view source_;
  std::size_t pos_ = 0;
};

struct Expr
{
  virtual ~Expr() = default;
  virtual auto eval_int(const std::array<std::vector<Card>, kHands>& hands) const
      -> int = 0;
};

struct BoolExpr
{
  virtual ~BoolExpr() = default;
  virtual auto eval_bool(const std::array<std::vector<Card>, kHands>& hands) const
      -> bool = 0;
};

struct IntLiteral final : Expr
{
  explicit IntLiteral(int value) : value_(value) {}

  auto eval_int(const std::array<std::vector<Card>, kHands>&) const -> int override
  {
    return value_;
  }

  int value_;
};

struct LenExpr final : Expr
{
  explicit LenExpr(std::unique_ptr<Expr> inner) : inner_(std::move(inner)) {}

  auto eval_int(const std::array<std::vector<Card>, kHands>& hands) const
      -> int override
  {
    return inner_->eval_int(hands);
  }

  std::unique_ptr<Expr> inner_;
};

struct HandHcpExpr final : Expr
{
  explicit HandHcpExpr(int seat) : seat_(seat) {}

  auto eval_int(const std::array<std::vector<Card>, kHands>& hands) const
      -> int override
  {
    int points = 0;
    for (const auto& card : hands[static_cast<std::size_t>(seat_)])
    {
      switch (card.rank)
      {
        case 14:
          points += 4;
          break;
        case 13:
          points += 3;
          break;
        case 12:
          points += 2;
          break;
        case 11:
          points += 1;
          break;
        default:
          break;
      }
    }
    return points;
  }

  int seat_;
};

struct HandSuitLengthExpr final : Expr
{
  HandSuitLengthExpr(int seat, int suit) : seat_(seat), suit_(suit) {}

  auto eval_int(const std::array<std::vector<Card>, kHands>& hands) const
      -> int override
  {
    int count = 0;
    for (const auto& card : hands[static_cast<std::size_t>(seat_)])
    {
      if (card.suit == suit_)
      {
        ++count;
      }
    }
    return count;
  }

  int seat_;
  int suit_;
};

struct HandShapeIndexExpr final : Expr
{
  HandShapeIndexExpr(int seat, int index) : seat_(seat), index_(index) {}

  auto eval_int(const std::array<std::vector<Card>, kHands>& hands) const
      -> int override
  {
    int counts[kSuits] = {0, 0, 0, 0};
    for (const auto& card : hands[static_cast<std::size_t>(seat_)])
    {
      ++counts[card.suit];
    }
    return counts[index_];
  }

  int seat_;
  int index_;
};

enum class CompareOp
{
  eq,
  ne,
  ge,
  le,
  gt,
  lt,
};

struct CompareExpr final : BoolExpr
{
  CompareExpr(
      std::unique_ptr<Expr> left,
      CompareOp op,
      std::unique_ptr<Expr> right)
      : left_(std::move(left)), op_(op), right_(std::move(right))
  {
  }

  auto eval_bool(const std::array<std::vector<Card>, kHands>& hands) const
      -> bool override
  {
    const int left = left_->eval_int(hands);
    const int right = right_->eval_int(hands);
    switch (op_)
    {
      case CompareOp::eq:
        return left == right;
      case CompareOp::ne:
        return left != right;
      case CompareOp::ge:
        return left >= right;
      case CompareOp::le:
        return left <= right;
      case CompareOp::gt:
        return left > right;
      case CompareOp::lt:
        return left < right;
    }
    return false;
  }

  std::unique_ptr<Expr> left_;
  CompareOp op_;
  std::unique_ptr<Expr> right_;
};

struct AndExpr final : BoolExpr
{
  AndExpr(std::unique_ptr<BoolExpr> left, std::unique_ptr<BoolExpr> right)
      : left_(std::move(left)), right_(std::move(right))
  {
  }

  auto eval_bool(const std::array<std::vector<Card>, kHands>& hands) const
      -> bool override
  {
    return left_->eval_bool(hands) && right_->eval_bool(hands);
  }

  std::unique_ptr<BoolExpr> left_;
  std::unique_ptr<BoolExpr> right_;
};

struct OrExpr final : BoolExpr
{
  OrExpr(std::unique_ptr<BoolExpr> left, std::unique_ptr<BoolExpr> right)
      : left_(std::move(left)), right_(std::move(right))
  {
  }

  auto eval_bool(const std::array<std::vector<Card>, kHands>& hands) const
      -> bool override
  {
    return left_->eval_bool(hands) || right_->eval_bool(hands);
  }

  std::unique_ptr<BoolExpr> left_;
  std::unique_ptr<BoolExpr> right_;
};

struct NotExpr final : BoolExpr
{
  explicit NotExpr(std::unique_ptr<BoolExpr> inner) : inner_(std::move(inner)) {}

  auto eval_bool(const std::array<std::vector<Card>, kHands>& hands) const
      -> bool override
  {
    return !inner_->eval_bool(hands);
  }

  std::unique_ptr<BoolExpr> inner_;
};

class Parser
{
public:
  explicit Parser(std::string_view source)
      : lexer_(source), current_(lexer_.next())
  {
  }

  auto parse() -> std::unique_ptr<BoolExpr>
  {
    auto expr = parse_or();
    if (current_.kind != TokenKind::end)
    {
      throw std::invalid_argument("unexpected trailing text in deal constraint");
    }
    return expr;
  }

private:
  auto eat(TokenKind kind) -> void
  {
    if (current_.kind != kind)
    {
      throw std::invalid_argument("invalid deal constraint syntax");
    }
    current_ = lexer_.next();
  }

  auto parse_or() -> std::unique_ptr<BoolExpr>
  {
    auto left = parse_and();
    while (current_.kind == TokenKind::or_kw)
    {
      eat(TokenKind::or_kw);
      auto right = parse_and();
      left = std::make_unique<OrExpr>(std::move(left), std::move(right));
    }
    return left;
  }

  auto parse_and() -> std::unique_ptr<BoolExpr>
  {
    auto left = parse_not();
    while (current_.kind == TokenKind::and_kw)
    {
      eat(TokenKind::and_kw);
      auto right = parse_not();
      left = std::make_unique<AndExpr>(std::move(left), std::move(right));
    }
    return left;
  }

  auto parse_not() -> std::unique_ptr<BoolExpr>
  {
    if (current_.kind == TokenKind::not_kw)
    {
      eat(TokenKind::not_kw);
      return std::make_unique<NotExpr>(parse_not());
    }
    return parse_comparison();
  }

  auto parse_comparison() -> std::unique_ptr<BoolExpr>
  {
    auto left = parse_value();
    if (current_.kind == TokenKind::eq || current_.kind == TokenKind::ne
        || current_.kind == TokenKind::ge || current_.kind == TokenKind::le
        || current_.kind == TokenKind::gt || current_.kind == TokenKind::lt)
    {
      const CompareOp op = [this]() {
        switch (current_.kind)
        {
          case TokenKind::eq:
            return CompareOp::eq;
          case TokenKind::ne:
            return CompareOp::ne;
          case TokenKind::ge:
            return CompareOp::ge;
          case TokenKind::le:
            return CompareOp::le;
          case TokenKind::gt:
            return CompareOp::gt;
          case TokenKind::lt:
            return CompareOp::lt;
          default:
            throw std::invalid_argument("invalid deal constraint syntax");
        }
      }();
      eat(current_.kind);
      auto right = parse_value();
      return std::make_unique<CompareExpr>(std::move(left), op, std::move(right));
    }
    throw std::invalid_argument(
        "deal constraint must be a comparison or boolean expression");
  }

  auto parse_value() -> std::unique_ptr<Expr>
  {
    if (current_.kind == TokenKind::integer)
    {
      const int value = current_.integer;
      eat(TokenKind::integer);
      return std::make_unique<IntLiteral>(value);
    }
    if (current_.kind == TokenKind::identifier && current_.text == "len")
    {
      eat(TokenKind::identifier);
      eat(TokenKind::lparen);
      auto inner = parse_deal_access();
      eat(TokenKind::rparen);
      return std::make_unique<LenExpr>(std::move(inner));
    }
    if (current_.kind == TokenKind::identifier && current_.text == "deal")
    {
      return parse_deal_access();
    }
    throw std::invalid_argument("invalid deal constraint value");
  }

  auto parse_deal_access() -> std::unique_ptr<Expr>
  {
    if (current_.kind != TokenKind::identifier || current_.text != "deal")
    {
      throw std::invalid_argument("expected deal.<seat>.<attribute>");
    }
    eat(TokenKind::identifier);
    eat(TokenKind::dot);
    const int seat = parse_seat_name();
    eat(TokenKind::dot);
    if (current_.kind != TokenKind::identifier)
    {
      throw std::invalid_argument("expected deal.<seat>.<attribute>");
    }
    const std::string attr = current_.text;
    eat(TokenKind::identifier);
    if (attr == "hcp")
    {
      return std::make_unique<HandHcpExpr>(seat);
    }
    if (attr == "shape")
    {
      eat(TokenKind::lbracket);
      if (current_.kind != TokenKind::integer)
      {
        throw std::invalid_argument("expected deal.<seat>.shape[index]");
      }
      const int index = current_.integer;
      if (index < 0 || index >= kSuits)
      {
        throw std::invalid_argument("shape index must be in 0..3");
      }
      eat(TokenKind::integer);
      eat(TokenKind::rbracket);
      return std::make_unique<HandShapeIndexExpr>(seat, index);
    }
    const std::optional<int> suit = parse_suit_name(attr);
    if (!suit.has_value())
    {
      throw std::invalid_argument("unknown hand attribute: " + attr);
    }
    return std::make_unique<HandSuitLengthExpr>(seat, *suit);
  }

  auto parse_seat_name() -> int
  {
    if (current_.kind != TokenKind::identifier)
    {
      throw std::invalid_argument("expected seat name");
    }
    const std::string seat = current_.text;
    eat(TokenKind::identifier);
    if (seat == "north")
    {
      return 0;
    }
    if (seat == "east")
    {
      return 1;
    }
    if (seat == "south")
    {
      return 2;
    }
    if (seat == "west")
    {
      return 3;
    }
    throw std::invalid_argument("unknown seat name: " + seat);
  }

  auto parse_suit_name(const std::string& name) -> std::optional<int>
  {
    if (name == "spades")
    {
      return 0;
    }
    if (name == "hearts")
    {
      return 1;
    }
    if (name == "diamonds")
    {
      return 2;
    }
    if (name == "clubs")
    {
      return 3;
    }
    return std::nullopt;
  }

  Lexer lexer_;
  Token current_;
};

auto compile_constraint_evaluator(
    std::string_view expression)
    -> std::function<bool(const std::array<std::vector<Card>, kHands>&)>
{
  const std::shared_ptr<BoolExpr> tree = Parser(expression).parse();
  return [tree](const std::array<std::vector<Card>, kHands>& hands) {
    return tree->eval_bool(hands);
  };
}

}  // namespace

struct DealConstraint::Impl
{
  explicit Impl(std::string_view expression)
      : evaluate(compile_constraint_evaluator(expression))
  {
  }

  std::function<bool(const std::array<std::vector<Card>, kHands>&)> evaluate;
};

DealConstraint::DealConstraint() = default;

DealConstraint::DealConstraint(std::string_view expression)
    : impl_(std::make_shared<const Impl>(expression))
{
}

auto DealConstraint::is_active() const -> bool
{
  return static_cast<bool>(impl_);
}

auto DealConstraint::evaluate(
    const std::array<std::vector<Card>, kHands>& hands) const -> bool
{
  if (!impl_)
  {
    return true;
  }
  return impl_->evaluate(hands);
}

auto compile_deal_constraint(std::string_view expression) -> DealConstraint
{
  if (expression.empty())
  {
    return DealConstraint{};
  }
  return DealConstraint(expression);
}

}  // namespace single_dummy_solver
