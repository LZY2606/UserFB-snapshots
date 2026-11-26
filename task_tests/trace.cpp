// Task tests for the optional fragment execution tracer.
//
// Three groups (registered below):
//   catalog   - trace catalog contents, buffer reflection and failure modes
//   execution - a small CPU bridge that actually interprets the generated
//               GLSL and checks event order, payloads, capacity and side effects
//   compat    - traced vs untraced equivalence, region clipping, names/bindings
//               and result/context lifetime
//
// The bridge parses and executes the real GLSL emitted by the library; it never
// recomputes the original source expression to stand in for the generated code.
#include "task_support/check.h"
#include "glsl_optimizer.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// ===========================================================================
// Minimal GLSL 430 interpreter covering exactly the syntax emitted by the
// traced/untraced sample shaders: scalar/vector float/int/uint/bool values,
// swizzles, vecN/int/uint/float/floor/floatBitsToUint, arithmetic, comparisons,
// if/else, for/while, break, discard and uint SSBO array access.
// ===========================================================================
namespace bridge {

enum Base { F32, I32, U32, BOOL };

static uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static float u2f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

struct Value {
   Base base = F32;
   int n = 1;
   uint32_t w[4] = {};

   static Value scalar(Base b, uint32_t bits) { Value v; v.base = b; v.n = 1; v.w[0] = bits; return v; }
   static Value from_float(float f) { return scalar(F32, f2u(f)); }
   static Value from_int(int32_t i) { return scalar(I32, (uint32_t)i); }
   static Value from_uint(uint32_t u) { return scalar(U32, u); }
   static Value from_bool(bool b) { return scalar(BOOL, b ? 1u : 0u); }
   static Value vec(Base b, int comps) { Value v; v.base = b; v.n = comps; return v; }
   float as_float() const { return u2f(w[0]); }
   int32_t as_int() const { return (int32_t)w[0]; }
   uint32_t as_uint() const { return w[0]; }
   bool as_bool() const { return w[0] != 0; }
};

static uint32_t convert(Base dst, const Value& v)
{
   switch (dst) {
   case F32:
      if (v.base == F32) return v.w[0];
      if (v.base == U32) return f2u((float)v.as_uint());
      if (v.base == I32) return f2u((float)v.as_int());
      return f2u(v.as_bool() ? 1.f : 0.f);
   case U32:
      if (v.base == U32) return v.w[0];
      if (v.base == I32) return (uint32_t)v.as_int();
      if (v.base == F32) return (uint32_t)v.as_float();
      return v.as_bool() ? 1u : 0u;
   case I32:
      if (v.base == I32) return v.w[0];
      if (v.base == U32) return (int32_t)v.as_uint();
      if (v.base == F32) return (int32_t)v.as_float();
      return v.as_bool() ? 1 : 0;
   default:
      return v.as_bool() ? 1u : 0u;
   }
}

// ---------------------------------------------------------------------------
// lexer
// ---------------------------------------------------------------------------
struct Tok {
   enum Kind { NUM, ID, OP } kind;
   std::string text;
   bool is_uint = false, is_float = false;
};

struct Lexer {
   const std::string& s;
   size_t p = 0;
   explicit Lexer(const std::string& src) : s(src) {}
   void spaces()
   {
      while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\t' || s[p] == '\r')) ++p;
   }
   std::vector<Tok> lex()
   {
      std::vector<Tok> out;
      static const char* two[] = {"==", "!=", "<=", ">=", "&&", "||", "++", "--", "<<", ">>"};
      while (p < s.size()) {
         spaces();
         if (p >= s.size()) break;
         char c = s[p];
         if (std::isdigit((unsigned char)c) || (c == '.' && p + 1 < s.size() && std::isdigit((unsigned char)s[p + 1]))) {
            size_t a = p;
            bool isf = false;
            while (p < s.size() && (std::isdigit((unsigned char)s[p]) || s[p] == '.' || s[p] == 'e' || s[p] == 'E' ||
                                    ((s[p] == '+' || s[p] == '-') && p > a && (s[p - 1] == 'e' || s[p - 1] == 'E')))) {
               if (s[p] == '.' || s[p] == 'e' || s[p] == 'E') isf = true;
               ++p;
            }
            bool isu = false;
            if (p < s.size() && (s[p] == 'u' || s[p] == 'U')) { isu = true; ++p; }
            if (p < s.size() && (s[p] == 'f' || s[p] == 'F')) { isf = true; ++p; }
            out.push_back({Tok::NUM, s.substr(a, p - a), isu, isf});
            continue;
         }
         if (std::isalnum((unsigned char)c) || c == '_') {
            size_t a = p;
            while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '_')) ++p;
            out.push_back({Tok::ID, s.substr(a, p - a), false, false});
            continue;
         }
         bool matched = false;
         for (const char* op : two) {
            if (s.compare(p, 2, op) == 0) { out.push_back({Tok::OP, op, false, false}); p += 2; matched = true; break; }
         }
         if (matched) continue;
         out.push_back({Tok::OP, std::string(1, c), false, false});
         ++p;
      }
      return out;
   }
};

// ---------------------------------------------------------------------------
// AST
// ---------------------------------------------------------------------------
struct Expr {
   enum { LIT, REF, INDEX, UNARY, BINARY, CALL } kind = LIT;
   Value lit;
   std::string name;                       // REF variable / CALL function / INDEX buffer
   std::string field;                      // INDEX struct field
   std::vector<int> swizzle;               // REF component indices
   std::string op;                         // UNARY/BINARY operator
   std::vector<Expr> args;                 // CALL args, BINARY operands or UNARY operand
};

struct Stmt {
   enum { DECL, ASSIGN, EXPR, IFSTMT, FORSTMT, WHILESTMT, BREAKSTMT, DISCARDSTMT } kind = EXPR;
   // declaration
   std::string dname; Base dtype = F32; int dn = 1; bool dinit = false; Expr dexpr;
   // assignment / generic target
   enum Target { T_VAR, T_COLOR, T_ARRAY } target = T_VAR;
   std::string tname, tfield;
   std::vector<int> tcomps;
   Expr tidx;
   Expr rhs;
   Expr expr;                             // EXPR (e.g. postfix increment)
   // control flow
   Expr cond;
   std::vector<Stmt> block, thenb, elseb, body;
   bool has_init = false, has_step = false;
   std::unique_ptr<Stmt> init, step;
};

struct Machine {
   std::vector<Tok> tk;
   size_t pos = 0;
   std::map<std::string, Value> vars;
   std::map<std::string, std::vector<uint32_t>*> bufs;
   std::map<std::string, std::vector<uint32_t>> storage;
   Value frag_coord = Value::vec(F32, 4);
   Value color = Value::vec(F32, 4);
   bool discarded = false, broke = false;

   const Tok& peek(int off = 0) const { return tk[pos + off]; }
   bool eof() const { return pos >= tk.size(); }
   const Tok& consume() { return tk[pos++]; }
   bool is_op(const char* op) const { return !eof() && peek().kind == Tok::OP && peek().text == op; }
   bool is_id(const char* id) const { return !eof() && peek().kind == Tok::ID && peek().text == id; }
   void expect_op(const char* op) { if (!is_op(op)) std::fprintf(stderr, "bridge: expected %s\n", op); else ++pos; }
   bool consume_op(const char* op) { if (is_op(op)) { ++pos; return true; } return false; }

   static int swiz(char c) { return (int)(std::strchr("xyzw", c) - "xyzw"); }

   Value number(const Tok& t) const
   {
      if (t.is_float) return Value::from_float(std::strtof(t.text.c_str(), nullptr));
      if (t.is_uint) return Value::from_uint((uint32_t)std::strtoul(t.text.c_str(), nullptr, 10));
      return Value::from_int((int32_t)std::strtol(t.text.c_str(), nullptr, 10));
   }

   // ----- expression parsing -----
   Expr parse_expr() { return parse_binary(0); }

   Expr parse_binary(int min_prec)
   {
      Expr left = parse_unary();
      while (!eof() && peek().kind == Tok::OP) {
         int pr = precedence(peek().text);
         if (pr == 0 || pr < min_prec) break;
         std::string op = consume().text;
         Expr right = parse_binary(pr + 1);
         Expr node; node.kind = Expr::BINARY; node.op = op;
         node.args.push_back(std::move(left)); node.args.push_back(std::move(right));
         left = std::move(node);
      }
      return left;
   }
   static int precedence(const std::string& op)
   {
      if (op == "||") return 1;
      if (op == "&&") return 2;
      if (op == "==" || op == "!=") return 3;
      if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
      if (op == "<<" || op == ">>") return 5;
      if (op == "+" || op == "-") return 6;
      if (op == "*" || op == "/") return 7;
      return 0;
   }

   Expr parse_unary()
   {
      if (is_op("-") || is_op("!") || is_op("++") || is_op("--")) {
         std::string op = consume().text;
         Expr node; node.kind = Expr::UNARY; node.op = op; node.args.push_back(parse_unary());
         return node;
      }
      Expr e = parse_postfix(parse_primary());
      if (is_op("++") || is_op("--")) {
         std::string op = consume().text;
         Expr node; node.kind = Expr::UNARY; node.op = "post" + op; node.args.push_back(std::move(e));
         return node;
      }
      return e;
   }

   Expr parse_primary()
   {
      const Tok& t = peek();
      if (t.kind == Tok::NUM) { consume(); Expr e; e.kind = Expr::LIT; e.lit = number(t); return e; }
      if (is_op("(")) { consume(); Expr e = parse_expr(); expect_op(")"); return e; }
      // identifier: variable, buffer field, boolean literal or call
      std::string name = consume().text;
      if (name == "true" || name == "false") { Expr e; e.kind = Expr::LIT; e.lit = Value::from_bool(name == "true"); return e; }
      if (is_op("(")) {
         consume();
         Expr e; e.kind = Expr::CALL; e.name = name;
         if (!is_op(")")) { e.args.push_back(parse_expr()); while (consume_op(",")) e.args.push_back(parse_expr()); }
         expect_op(")");
         return e;
      }
      Expr e; e.kind = Expr::REF; e.name = name;
      return e;
   }

   Expr parse_postfix(Expr e)
   {
      while (true) {
         if (is_op(".")) {
            consume();
            std::string member = consume().text;
            if (is_op("[")) {
               // SSBO struct field index: buffer.field[index]
               consume();
               Expr idx = parse_expr();
               expect_op("]");
               Expr node; node.kind = Expr::INDEX; node.name = e.name; node.field = member;
               node.args.push_back(std::move(idx));
               e = std::move(node);
            } else {
               for (char c : member) e.swizzle.push_back(swiz(c));
            }
         } else if (is_op("[")) {
            consume();
            Expr idx = parse_expr();
            expect_op("]");
            Expr node; node.kind = Expr::INDEX; node.name = e.name;
            node.args.push_back(std::move(idx));
            e = std::move(node);
         } else break;
      }
      return e;
   }

   // ----- evaluation -----
   Value eval(const Expr& e)
   {
      switch (e.kind) {
      case Expr::LIT: return e.lit;
      case Expr::REF: {
         Value v;
         if (e.name == "gl_FragCoord") v = frag_coord;
         else v = vars[e.name];
         if (!e.swizzle.empty()) {
            Value r = Value::vec(v.base, (int)e.swizzle.size());
            for (size_t i = 0; i < e.swizzle.size(); i++) r.w[i] = v.w[e.swizzle[i]];
            return r;
         }
         return v;
      }
      case Expr::INDEX: {
         uint32_t k = convert(U32, eval(e.args[0]));
         return Value::from_uint((*bufs.at(e.name))[k]);
      }
      case Expr::UNARY: {
         if (e.op == "post++" || e.op == "post--") {
            Value before = eval(e.args[0]);
            int d = e.op == "post++" ? 1 : -1;
            write_lvalue(e.args[0], Value::scalar(before.base, before.w[0] + (before.base == F32 ? f2u(before.as_float() + (float)d) : 1u * d)));
            return before;
         }
         Value v = eval(e.args[0]);
         if (e.op == "-") {
            Value r = Value::vec(v.base, v.n);
            for (int i = 0; i < v.n; i++)
               r.w[i] = v.base == F32 ? f2u(-v.as_float()) : (uint32_t)(-v.as_int());
            return r;
         }
         if (e.op == "!") return Value::from_bool(!v.as_bool());
         if (e.op == "++" || e.op == "--") {
            int d = e.op == "++" ? 1 : -1;
            Value nv = Value::scalar(v.base, v.w[0] + (v.base == F32 ? f2u(v.as_float() + (float)d) : (uint32_t)d));
            write_lvalue(e.args[0], nv);
            return nv;
         }
         return v;
      }
      case Expr::BINARY: return eval_binop(e);
      case Expr::CALL: return eval_call(e);
      }
      return Value::from_float(0.f);
   }

   Value eval_binop(const Expr& e)
   {
      const std::string& op = e.op;
      if (op == "&&") return Value::from_bool(eval(e.args[0]).as_bool() && eval(e.args[1]).as_bool());
      if (op == "||") return Value::from_bool(eval(e.args[0]).as_bool() || eval(e.args[1]).as_bool());
      Value a = eval(e.args[0]), b = eval(e.args[1]);
      if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
         bool r;
         if (a.base == F32 || b.base == F32) {
            float x = u2f(convert(F32, a)), y = u2f(convert(F32, b));
            r = op == "==" ? x == y : op == "!=" ? x != y : op == "<" ? x < y : op == "<=" ? x <= y : op == ">" ? x > y : x >= y;
         } else if (a.base == U32 || b.base == U32) {
            uint32_t x = convert(U32, a), y = convert(U32, b);
            r = op == "==" ? x == y : op == "!=" ? x != y : op == "<" ? x < y : op == "<=" ? x <= y : op == ">" ? x > y : x >= y;
         } else {
            int32_t x = convert(I32, a), y = convert(I32, b);
            r = op == "==" ? x == y : op == "!=" ? x != y : op == "<" ? x < y : op == "<=" ? x <= y : op == ">" ? x > y : x >= y;
         }
         return Value::from_bool(r);
      }
      if (op == "<<") return Value::from_uint(convert(U32, a) << (convert(U32, b) & 31u));
      if (op == ">>") return Value::from_uint(convert(U32, a) >> (convert(U32, b) & 31u));
      Base dst = (a.base == F32 || b.base == F32) ? F32 : (a.base == U32 || b.base == U32 ? U32 : I32);
      Value r = Value::vec(dst, 1);
      if (dst == F32) {
         float x = u2f(convert(F32, a)), y = u2f(convert(F32, b));
         float z = op == "+" ? x + y : op == "-" ? x - y : op == "*" ? x * y : x / y;
         r.w[0] = f2u(z);
      } else if (dst == U32) {
         uint32_t x = convert(U32, a), y = convert(U32, b);
         r.w[0] = op == "+" ? x + y : op == "-" ? x - y : op == "*" ? x * y : x / y;
      } else {
         int32_t x = convert(I32, a), y = convert(I32, b);
         int32_t z = op == "+" ? x + y : op == "-" ? x - y : op == "*" ? x * y : x / y;
         r.w[0] = (uint32_t)z;
      }
      return r;
   }

   Value eval_call(const Expr& e)
   {
      const std::string& fn = e.name;
      if (fn == "int") return Value::from_int((int32_t)convert(I32, eval(e.args[0])));
      if (fn == "uint") return Value::from_uint(convert(U32, eval(e.args[0])));
      if (fn == "float") return Value::from_float(u2f(convert(F32, eval(e.args[0]))));
      if (fn == "floor") return Value::from_float(std::floor(eval(e.args[0]).as_float()));
      if (fn == "floatBitsToUint") return Value::from_uint(eval(e.args[0]).w[0]);
      if (fn == "vec2" || fn == "vec3" || fn == "vec4") {
         int comps = fn[3] - '0';
         Value r = Value::vec(F32, comps);
         if (e.args.size() == 1) {
            Value av = eval(e.args[0]);
            if (av.n == comps) { for (int i = 0; i < comps; i++) r.w[i] = av.w[i]; return r; }
            // scalar argument broadcasts to every component
            uint32_t scalar = convert(F32, av);
            for (int i = 0; i < comps; i++) r.w[i] = scalar;
            return r;
         }
         int k = 0;
         for (const Expr& ae : e.args) {
            Value av = eval(ae);
            for (int i = 0; i < av.n && k < comps; i++) r.w[k++] = convert(F32, av);
         }
         return r;
      }
      std::fprintf(stderr, "bridge: unknown call %s\n", fn.c_str());
      return Value::from_float(0.f);
   }

   // ----- lvalues -----
   void write_lvalue(const Expr& target, const Value& rv)
   {
      if (target.kind == Expr::INDEX) {
         uint32_t k = convert(U32, eval(target.args[0]));
         (*bufs.at(target.name))[k] = rv.w[0];
         return;
      }
      // REF, possibly a color/variable swizzle
      if (target.name == "color") {
         if (target.swizzle.empty()) { for (int i = 0; i < rv.n; i++) color.w[i] = rv.w[i]; }
         else for (size_t i = 0; i < target.swizzle.size(); i++) color.w[target.swizzle[i]] = rv.w[i];
         return;
      }
      Value& v = vars[target.name];
      if (target.swizzle.empty()) { v = rv; return; }
      for (size_t i = 0; i < target.swizzle.size(); i++) v.w[target.swizzle[i]] = rv.w[i];
   }

   // ----- statements -----
   Stmt parse_lvalue_stmt()
   {
      Stmt st; st.kind = Stmt::ASSIGN;
      Expr target = parse_postfix(parse_primary());
      if (target.kind == Expr::INDEX) {
         st.target = Stmt::T_ARRAY; st.tname = target.name; st.tfield = target.field;
         st.tidx = target.args[0];
      } else {
         st.target = target.name == "color" ? Stmt::T_COLOR : Stmt::T_VAR;
         st.tname = target.name; st.tcomps = target.swizzle;
      }
      expect_op("=");
      st.rhs = parse_expr();
      expect_op(";");
      return st;
   }

   Base type_keyword(int* comps)
   {
      *comps = 1;
      std::string kw = consume().text;
      if (kw.size() == 4 && kw[0] == 'v' && kw[2] == 'c') { *comps = kw[3] - '0'; return kw[1] == 'i' ? I32 : kw[1] == 'u' ? U32 : F32; }
      if (kw == "float") return F32;
      if (kw == "int") return I32;
      if (kw == "uint") return U32;
      return BOOL;
   }

   std::vector<Stmt> parse_block()
   {
      expect_op("{");
      std::vector<Stmt> stmts;
      while (!consume_op("}")) {
         if (eof()) break;
         stmts.push_back(parse_stmt());
      }
      return stmts;
   }

   Stmt parse_stmt()
   {
      if (is_op("{")) { Stmt s; s.kind = Stmt::EXPR; s.block = parse_block(); return s; }
      if (is_id("if")) return parse_if();
      if (is_id("for")) return parse_for();
      if (is_id("while")) return parse_while();
      if (is_id("break")) { consume(); expect_op(";"); Stmt s; s.kind = Stmt::BREAKSTMT; return s; }
      if (is_id("discard")) { consume(); expect_op(";"); Stmt s; s.kind = Stmt::DISCARDSTMT; return s; }
      if (is_id("float") || is_id("int") || is_id("uint") || is_id("bool") ||
          (peek().kind == Tok::ID && peek().text.size() == 4 && peek().text[0] == 'v' && peek().text[2] == 'c')) {
         Stmt s; s.kind = Stmt::DECL;
         s.dtype = type_keyword(&s.dn);
         s.dname = consume().text;
         if (consume_op("=")) { s.dinit = true; s.dexpr = parse_expr(); }
         expect_op(";");
         return s;
      }
      // assignment or expression statement
      size_t save = pos;
      Expr first = parse_expr();
      if (is_op("=")) {
         Stmt s; s.kind = Stmt::ASSIGN;
         if (first.kind == Expr::INDEX) {
            s.target = Stmt::T_ARRAY; s.tname = first.name; s.tfield = first.field; s.tidx = first.args[0];
         } else {
            s.target = first.name == "color" ? Stmt::T_COLOR : Stmt::T_VAR;
            s.tname = first.name; s.tcomps = first.swizzle;
         }
         consume();
         s.rhs = parse_expr();
         expect_op(";");
         return s;
      }
      pos = save;
      parse_expr();
      expect_op(";");
      Stmt s; s.kind = Stmt::EXPR; s.expr = first;
      return s;
   }

   Stmt parse_if()
   {
      consume(); // if
      expect_op("(");
      Stmt s; s.kind = Stmt::IFSTMT; s.cond = parse_expr(); expect_op(")");
      s.thenb = parse_block();
      if (is_id("else")) { consume(); if (is_op("{")) s.elseb = parse_block(); else { std::vector<Stmt> one; one.push_back(parse_stmt()); s.elseb = std::move(one); } }
      return s;
   }

   Stmt parse_while()
   {
      consume(); // while
      expect_op("(");
      Stmt s; s.kind = Stmt::WHILESTMT; s.cond = parse_expr(); expect_op(")");
      s.body = parse_block();
      return s;
   }

   Stmt parse_for()
   {
      consume(); // for
      expect_op("(");
      Stmt s; s.kind = Stmt::FORSTMT;
      if (!is_op(";")) { s.has_init = true; s.init = std::make_unique<Stmt>(parse_stmt_no_semicolon_is_inside()); }
      expect_op(";");
      if (!is_op(";")) s.cond = parse_expr();
      expect_op(";");
      if (!is_op(")")) { s.has_step = true; s.step = std::make_unique<Stmt>(parse_step()); }
      expect_op(")");
      s.body = parse_block();
      return s;
   }
   Stmt parse_stmt_no_semicolon_is_inside()
   {
      // init is an assignment (the generated loops predeclare the counter)
      Stmt s; s.kind = Stmt::ASSIGN;
      Expr target = parse_expr();
      expect_op("=");
      if (target.kind == Expr::INDEX) { s.target = Stmt::T_ARRAY; s.tname = target.name; s.tidx = target.args[0]; }
      else { s.target = target.name == "color" ? Stmt::T_COLOR : Stmt::T_VAR; s.tname = target.name; s.tcomps = target.swizzle; }
      s.rhs = parse_expr();
      return s;
   }
   Stmt parse_step()
   {
      Stmt s; s.kind = Stmt::EXPR; s.expr = parse_expr();
      return s;
   }

   void exec(const std::vector<Stmt>& stmts)
   {
      for (const Stmt& s : stmts) exec_stmt(s);
   }

   void exec_stmt(const Stmt& s)
   {
      if (discarded || broke) return;
      switch (s.kind) {
      case Stmt::DECL: {
         Value v = Value::vec(s.dtype, s.dn);
         if (s.dinit) {
            Value iv = eval(s.dexpr);
            for (int i = 0; i < s.dn; i++) v.w[i] = convert(s.dtype, i < iv.n ? Value::scalar(iv.base, iv.w[i]) : iv);
         }
         vars[s.dname] = v;
         break;
      }
      case Stmt::ASSIGN: {
         Value rv = eval(s.rhs);
         if (s.target == Stmt::T_ARRAY) {
            uint32_t k = convert(U32, eval(s.tidx));
            (*bufs.at(s.tname))[k] = convert(U32, rv);
         } else if (s.target == Stmt::T_COLOR) {
            if (s.tcomps.empty()) { for (int i = 0; i < rv.n; i++) color.w[i] = rv.w[i]; }
            else for (size_t i = 0; i < s.tcomps.size(); i++) color.w[s.tcomps[i]] = rv.w[i];
         } else {
            Value& v = vars[s.tname];
            if (s.tcomps.empty()) v = rv;
            else for (size_t i = 0; i < s.tcomps.size(); i++) v.w[s.tcomps[i]] = rv.w[i];
         }
         break;
      }
      case Stmt::EXPR:
         if (!s.block.empty()) exec(s.block);
         else eval(s.expr);
         break;
      case Stmt::IFSTMT:
         if (eval(s.cond).as_bool()) exec(s.thenb);
         else exec(s.elseb);
         break;
      case Stmt::WHILESTMT:
         while (eval(s.cond).as_bool()) { exec(s.body); if (broke) { broke = false; break; } }
         break;
      case Stmt::FORSTMT: {
         if (s.has_init) exec_stmt(*s.init);
         while (true) {
            if (!eval(s.cond).as_bool()) break;
            exec(s.body);
            if (broke) { broke = false; break; }
            if (s.has_step) exec_stmt(*s.step);
         }
         break;
      }
      case Stmt::BREAKSTMT: broke = true; break;
      case Stmt::DISCARDSTMT: discarded = true; break;
      }
   }
};

// Buffer instances are the identifiers following "}" in the global section.
static std::vector<std::string> buffer_instances(const std::string& src, size_t main_pos)
{
   std::vector<std::string> names;
   const std::string prefix = src.substr(0, main_pos);
   for (size_t i = 0; i + 1 < prefix.size(); i++) {
      if (prefix[i] == '}') {
         size_t j = i + 1;
         while (j < prefix.size() && prefix[j] == ' ') j++;
         size_t a = j;
         while (j < prefix.size() && (std::isalnum((unsigned char)prefix[j]) || prefix[j] == '_')) j++;
         if (j > a) names.push_back(prefix.substr(a, j - a));
      }
   }
   return names;
}

struct RunResult {
   Value color = Value::vec(F32, 4);
   bool discarded = false;
   std::map<std::string, std::vector<uint32_t>> storage;
};

// Executes generated GLSL for one fragment. Buffer sizes are supplied by the
// caller; the trace buffer is pre-filled with a sentinel to detect clipping.
static RunResult
run_shader(const std::string& src, float fx, float fy, const std::map<std::string, Value>& uniforms,
           const std::map<std::string, size_t>& buffer_sizes, const std::string& trace_name,
           uint32_t sentinel, uint32_t orig_init)
{
   size_t main_pos = src.find("void main");
   std::string body = src.substr(src.find("{", main_pos) + 1);
   body = body.substr(0, body.rfind("}"));

   Lexer lx(body);
   Machine m;
   m.frag_coord.w[0] = f2u(fx); m.frag_coord.w[1] = f2u(fy); m.frag_coord.w[2] = f2u(0.f); m.frag_coord.w[3] = f2u(1.f);
   for (auto& kv : uniforms) m.vars[kv.first] = kv.second;

   for (const std::string& nm : buffer_instances(src, main_pos)) {
      size_t sz = buffer_sizes.count(nm) ? buffer_sizes.at(nm) : 8;
      auto it = m.storage.emplace(nm, std::vector<uint32_t>(sz, sentinel)).first;
      m.bufs[nm] = &it->second;
   }
   // original SSBO starts from a known value rather than the sentinel
   for (auto& kv : m.storage)
      if (kv.first != trace_name) std::fill(kv.second.begin(), kv.second.end(), orig_init);

   std::vector<Tok> toks = lx.lex();
   m.tk = toks;
   std::vector<Stmt> program;
   while (!m.eof()) program.push_back(m.parse_stmt());
   m.exec(program);

   RunResult r;
   r.color = m.color;
   r.discarded = m.discarded;
   r.storage = std::move(m.storage);
   return r;
}

} // namespace bridge

// ===========================================================================
// task cases
// ===========================================================================
using bridge::Value;
using bridge::RunResult;
using bridge::f2u;
using bridge::u2f;

static const char* kSampleA =
   "#version 430\n"
   "layout(std430,binding=4) buffer Original { uint data[]; } orig;\n"
   "uniform float gain; uniform int n;\n"
   "out vec4 color;\n"
   "void main(){\n"
   "  float a = gain * 1.5 + 0.25;\n"
   "  for (int i=0;i<n;++i){ a = a*0.5 + float(i); orig.data[0] += 1u; }\n"
   "  float b = a - gain;\n"
   "  if (b > 0.0) { color.x = a; color.y = b; } else { color.x = -a; }\n"
   "  color.z = gain * 2.0;\n"
   "  if (gain < -10.0) discard;\n"
   "  color.w = 1.0;\n"
   "}\n";

static const char* kSampleB =
   "#version 430\n"
   "uniform vec4 u;\n"
   "out vec4 color;\n"
   "void main(){ color = vec4(u.x*2.0+0.5, u.y*2.0-0.5, u.z*3.0, -u.w); }\n";

struct Compiled {
   glslopt_ctx* ctx = nullptr;
   glslopt_shader* shader = nullptr;
   const glslopt_trace_info* info = nullptr;
};

static Compiled
compile_traced(const char* src, const glslopt_trace_config& cfg)
{
   Compiled c;
   c.ctx = glslopt_initialize(kGlslTargetOpenGL);
   c.shader = glslopt_optimize_traced(c.ctx, kGlslOptShaderFragment, src, 0, &cfg);
   if (glslopt_get_status(c.shader))
      c.info = glslopt_shader_get_trace_info(c.shader);
   return c;
}

struct Rec { uint32_t site, tag, p[4]; };

static size_t
pixel_base(const glslopt_trace_info* info, const glslopt_trace_config& cfg, float fx, float fy)
{
   int ix = (int)std::floor(fx) - cfg.origin_x;
   int iy = (int)std::floor(fy) - cfg.origin_y;
   return ((size_t)iy * cfg.width + ix) * info->segment_words;
}

static Rec
read_rec(const std::vector<uint32_t>& buf, size_t base, unsigned idx)
{
   size_t rp = base + 2 + (size_t)idx * 6;
   Rec r;
   r.site = buf[rp]; r.tag = buf[rp + 1];
   for (int i = 0; i < 4; i++) r.p[i] = buf[rp + 2 + i];
   return r;
}

// Finds the (occurrence)-th record written for a site id. Output and discard
// sites execute at most once; loop/branch sites can repeat.
static bool
find_record(const std::vector<uint32_t>& buf, size_t base, unsigned site_id, Rec* out, unsigned occurrence = 0)
{
   unsigned attempted = buf[base];
   unsigned seen = 0;
   for (unsigned i = 0; i < attempted; i++) {
      Rec rc = read_rec(buf, base, i);
      if (rc.site == site_id) {
         if (seen++ == occurrence) { *out = rc; return true; }
      }
   }
   return false;
}

static RunResult
run_traced(const Compiled& c, const glslopt_trace_config& cfg, float fx, float fy,
           const std::map<std::string, Value>& uniforms, uint32_t sentinel, uint32_t orig_init)
{
   std::map<std::string, size_t> sizes;
   sizes["orig"] = 16;
   sizes[c.info->buffer_name] = (size_t)cfg.width * cfg.height * c.info->segment_words;
   return bridge::run_shader(glslopt_get_output(c.shader), fx, fy, uniforms, sizes,
                             c.info->buffer_name, sentinel, orig_init);
}

// ---- catalog group --------------------------------------------------------

static int
case_catalog_contents()
{
   glslopt_trace_config cfg{9, 1, 1, 4, 4, 100};
   Compiled c = compile_traced(kSampleA, cfg);
   TASK_CHECK(glslopt_get_status(c.shader));
   TASK_CHECK(c.info != nullptr);
   TASK_CHECK(c.info->site_count == 16);
   TASK_CHECK(c.info->segment_words == 2 + 6 * 100);
   TASK_CHECK(c.info->binding == 9);

   bool saw_alu = false, saw_branch = false, saw_output = false, saw_discard = false;
   for (unsigned i = 0; i < c.info->site_count; i++) {
      const auto& s = c.info->sites[i];
      TASK_CHECK(s.id == i + 1); // stable, 1-based, contiguous
      TASK_CHECK(s.kind >= kGlslTraceAlu && s.kind <= kGlslTraceDiscard);
      TASK_CHECK(s.components <= 4);
      if (s.kind == kGlslTraceAlu) {
         saw_alu = true;
         TASK_CHECK(s.components >= 1 && s.components <= 4);
         TASK_CHECK(s.output_name == nullptr && s.write_mask == 0);
      }
      if (s.kind == kGlslTraceBranch) {
         saw_branch = true;
         TASK_CHECK(s.components == 1);
      }
      if (s.kind == kGlslTraceOutput) {
         saw_output = true;
         TASK_CHECK(s.output_name != nullptr && std::strcmp(s.output_name, "color") == 0);
         TASK_CHECK(s.write_mask != 0 && (s.write_mask & ~15u) == 0);
         TASK_CHECK(s.components == (unsigned)__builtin_popcount(s.write_mask));
      }
      if (s.kind == kGlslTraceDiscard) {
         saw_discard = true;
         TASK_CHECK(s.components == 0 && s.write_mask == 0);
      }
   }
   TASK_CHECK(saw_alu && saw_branch && saw_output && saw_discard);

   // tag formula on a few known sites from this shader
   const auto& discard_site = c.info->sites[14]; // id 15
   TASK_CHECK(discard_site.id == 15 && discard_site.kind == kGlslTraceDiscard);
   const auto& xy_site = c.info->sites[8]; // id 9, color.xy
   TASK_CHECK(xy_site.kind == kGlslTraceOutput && xy_site.write_mask == 3 && xy_site.components == 2);
   glslopt_shader_delete(c.shader);
   glslopt_cleanup(c.ctx);
   return 0;
}

static int
case_catalog_reflection()
{
   glslopt_trace_config cfg{9, 0, 0, 2, 2, 50};
   Compiled c = compile_traced(kSampleB, cfg);
   TASK_CHECK(glslopt_get_status(c.shader));
   const auto* info = c.info;
   TASK_CHECK(info->buffer_name && info->buffer_name[0] != 0);
   TASK_CHECK(info->buffer_type_name && info->buffer_type_name[0] != 0);
   TASK_CHECK(std::strcmp(info->buffer_name, "orig") != 0);

   int trace_index = -1;
   int count = glslopt_shader_get_buffer_count(c.shader);
   TASK_CHECK(count >= 1); // sample B has no SSBO of its own, only the trace buffer
   for (int i = 0; i < count; i++) {
      const auto* d = glslopt_shader_get_buffer_desc(c.shader, i);
      TASK_CHECK(d != nullptr);
      if (d->binding == 9 && d->kind == kGlslBufferStorage) trace_index = i;
   }
   TASK_CHECK(trace_index >= 0);
   const auto* td = glslopt_shader_get_buffer_desc(c.shader, trace_index);
   TASK_CHECK(td->layout == kGlslLayoutStd430);
   TASK_CHECK(td->instanceName && std::strcmp(td->instanceName, info->buffer_name) == 0);
   TASK_CHECK(td->name && std::strcmp(td->name, info->buffer_type_name) == 0);
   TASK_CHECK(td->size == -1); // runtime sized
   glslopt_shader_delete(c.shader);
   glslopt_cleanup(c.ctx);

   // with a user SSBO, the original binding is preserved and both reflect
   glslopt_trace_config cfgA{9, 1, 1, 4, 4, 100};
   Compiled a = compile_traced(kSampleA, cfgA);
   TASK_CHECK(glslopt_get_status(a.shader));
   int trace_a = -1, orig_a = -1;
   int acount = glslopt_shader_get_buffer_count(a.shader);
   TASK_CHECK(acount >= 2);
   for (int i = 0; i < acount; i++) {
      const auto* d = glslopt_shader_get_buffer_desc(a.shader, i);
      if (d->binding == 9 && d->kind == kGlslBufferStorage) trace_a = i;
      if (d->binding == 4 && d->kind == kGlslBufferStorage) orig_a = i;
   }
   TASK_CHECK(trace_a >= 0 && orig_a >= 0 && trace_a != orig_a);
   glslopt_shader_delete(a.shader);
   glslopt_cleanup(a.ctx);
   return 0;
}

static int
case_catalog_failures()
{
   glslopt_trace_config good{5, 0, 0, 2, 2, 4};

   // active SSBO binding clash
   {
      const char* s = "#version 430\nlayout(std430,binding=7) buffer B{uint d[];}b;\nout vec4 c;\nvoid main(){ b.d[0]=1u; c=vec4(1.0); }\n";
      glslopt_trace_config bad = good; bad.ssbo_binding = 7;
      Compiled c = compile_traced(s, bad);
      TASK_CHECK(!glslopt_get_status(c.shader));
      TASK_CHECK(std::strstr(glslopt_get_log(c.shader), "binding") != nullptr);
      glslopt_shader_delete(c.shader); glslopt_cleanup(c.ctx);
   }
   // a UBO on the same binding is allowed
   {
      const char* s = "#version 430\nlayout(std140,binding=7) uniform U{float x;}u;\nout vec4 c;\nvoid main(){ c=vec4(u.x); }\n";
      glslopt_trace_config ok = good; ok.ssbo_binding = 7;
      Compiled c = compile_traced(s, ok);
      TASK_CHECK(glslopt_get_status(c.shader));
      glslopt_shader_delete(c.shader); glslopt_cleanup(c.ctx);
   }
   // non-float output rejected
   {
      const char* s = "#version 430\nout int k;\nvoid main(){ k=3; }\n";
      Compiled c = compile_traced(s, good);
      TASK_CHECK(!glslopt_get_status(c.shader));
      glslopt_shader_delete(c.shader); glslopt_cleanup(c.ctx);
   }
   // non-positive dimensions rejected
   {
      glslopt_trace_config bad = good; bad.width = 0;
      Compiled c = compile_traced(kSampleB, bad);
      TASK_CHECK(!glslopt_get_status(c.shader));
      glslopt_shader_delete(c.shader); glslopt_cleanup(c.ctx);
   }
   // buffer too large for a signed 32-bit index
   {
      glslopt_trace_config bad{5, 0, 0, 0x10000, 0x10000, 0x100000};
      Compiled c = compile_traced(kSampleB, bad);
      TASK_CHECK(!glslopt_get_status(c.shader));
      glslopt_shader_delete(c.shader); glslopt_cleanup(c.ctx);
   }
   // wrong stage and wrong target fail
   {
      auto* ctx = glslopt_initialize(kGlslTargetOpenGL);
      const char* vs = "#version 430\nvoid main(){ gl_Position=vec4(1.0); }\n";
      auto* sh = glslopt_optimize_traced(ctx, kGlslOptShaderVertex, vs, 0, &good);
      TASK_CHECK(!glslopt_get_status(sh));
      glslopt_shader_delete(sh); glslopt_cleanup(ctx);
   }
   {
      auto* ctx = glslopt_initialize(kGlslTargetOpenGLES30);
      const char* es = "#version 320 es\nlayout(location=0) out highp vec4 c;\nvoid main(){ c=vec4(1.0); }\n";
      auto* sh = glslopt_optimize_traced(ctx, kGlslOptShaderFragment, es, 0, &good);
      TASK_CHECK(!glslopt_get_status(sh));
      glslopt_shader_delete(sh); glslopt_cleanup(ctx);
   }
   return 0;
}

// ---- execution group ------------------------------------------------------

static int
case_exec_float_payload()
{
   glslopt_trace_config cfg{9, 0, 0, 2, 2, 50};
   Compiled c = compile_traced(kSampleB, cfg);
   TASK_CHECK(glslopt_get_status(c.shader));

   std::map<std::string, Value> uni;
   Value u = Value::vec(bridge::F32, 4);
   u.w[0] = f2u(1.5f);
   u.w[1] = f2u(-2.25f);
   u.w[2] = 0x80000000u; // -0.0: keeps its sign through *3.0
   u.w[3] = f2u(0.0f);   //  -u.w must be negative zero
   uni["u"] = u;

   RunResult r = run_traced(c, cfg, 0, 0, uni, 0xDEADBEEFu, 7u);
   TASK_CHECK(!r.discarded);

   // exact asymmetric bit patterns the generated shader computed
   const uint32_t expected[4] = {0x40600000u, 0xc0a00000u, 0x80000000u, 0x80000000u};
   for (int i = 0; i < 4; i++)
      TASK_CHECK(r.color.w[i] == expected[i]);

   size_t base = pixel_base(c.info, cfg, 0, 0);
   const auto& buf = r.storage.at(c.info->buffer_name);
   TASK_CHECK(buf[base] == c.info->site_count); // one event per site
   TASK_CHECK(buf[base + 1] == 0);              // nothing dropped

   // the output site (last one, 4 components) carries the same raw bits
   const auto& out_site = c.info->sites[c.info->site_count - 1];
   TASK_CHECK(out_site.kind == kGlslTraceOutput && out_site.components == 4);
   Rec out_rec;
   TASK_CHECK(find_record(buf, base, out_site.id, &out_rec));
   TASK_CHECK(out_rec.site == out_site.id);
   TASK_CHECK(out_rec.tag == kGlslTraceOutput * 8 + 4);
   for (int i = 0; i < 4; i++) TASK_CHECK(out_rec.p[i] == expected[i]);

   // scalar ALU sites record float bit patterns, with zeroed spare payloads
   Rec alu2;
   TASK_CHECK(find_record(buf, base, 2, &alu2)); // site 2 = u.x*2+0.5 = 3.5
   TASK_CHECK(alu2.tag == kGlslTraceAlu * 8 + 1);
   TASK_CHECK(alu2.p[0] == 0x40600000u && alu2.p[1] == 0 && alu2.p[2] == 0 && alu2.p[3] == 0);
   glslopt_shader_delete(c.shader);
   glslopt_cleanup(c.ctx);
   return 0;
}

static int
case_exec_branch_loop_order()
{
   glslopt_trace_config cfg{9, 1, 1, 4, 4, 100};
   Compiled c = compile_traced(kSampleA, cfg);
   TASK_CHECK(glslopt_get_status(c.shader));

   std::map<std::string, Value> uni;
   uni["gain"] = Value::from_float(2.0f);
   uni["n"] = Value::from_int(3);
   RunResult r = run_traced(c, cfg, 2, 3, uni, 0xDEADBEEFu, 10u);
   TASK_CHECK(!r.discarded);

   size_t base = pixel_base(c.info, cfg, 2, 3);
   const auto& buf = r.storage.at(c.info->buffer_name);
   TASK_CHECK(buf[base] == 22); // total executed events
   TASK_CHECK(buf[base + 1] == 0);

   // dynamic event order: prologue, three loop rounds, exit test, tail
   const unsigned expected_sites[] = {
      1, 2,
      3, 4, 5, 6,  3, 4, 5, 6,  3, 4, 5, 6,  3,
      7, 8, 9, 12, 13, 14, 16};
   std::vector<Rec> recs;
   for (unsigned i = 0; i < buf[base]; i++) recs.push_back(read_rec(buf, base, i));
   TASK_CHECK(recs.size() == sizeof(expected_sites) / sizeof(expected_sites[0]));
   for (size_t i = 0; i < recs.size(); i++) TASK_CHECK(recs[i].site == expected_sites[i]);

   // branch site 3 is evaluated four times: false, false, false, true
   unsigned branch_round = 0;
   for (const Rec& rc : recs) {
      if (rc.site == 3) {
         TASK_CHECK(rc.tag == kGlslTraceBranch * 8 + 1);
         TASK_CHECK(rc.p[0] == (branch_round < 3 ? 0u : 1u));
         branch_round++;
      }
   }
   TASK_CHECK(branch_round == 4);
   // the other two branches: b>0 taken, gain<-10 not taken
   const Rec* site8 = nullptr;
   const Rec* site14 = nullptr;
   for (const Rec& rc : recs) {
      if (rc.site == 8) site8 = &rc;
      if (rc.site == 14) site14 = &rc;
   }
   TASK_CHECK(site8 && site8->p[0] == 1);
   TASK_CHECK(site14 && site14->p[0] == 0);
   glslopt_shader_delete(c.shader);
   glslopt_cleanup(c.ctx);
   return 0;
}

static int
case_exec_partial_output()
{
   glslopt_trace_config cfg{9, 1, 1, 4, 4, 100};
   Compiled c = compile_traced(kSampleA, cfg);
   TASK_CHECK(glslopt_get_status(c.shader));

   // else path of the b>0 selection: one-component output store
   std::map<std::string, Value> uni;
   uni["gain"] = Value::from_float(10.0f);
   uni["n"] = Value::from_int(1);
   RunResult r = run_traced(c, cfg, 2, 3, uni, 0xDEADBEEFu, 10u);
   size_t base = pixel_base(c.info, cfg, 2, 3);
   const auto& buf = r.storage.at(c.info->buffer_name);

   const auto& x_site = c.info->sites[10]; // id 11, color.x only
   TASK_CHECK(x_site.kind == kGlslTraceOutput && x_site.write_mask == 1 && x_site.components == 1);
   Rec x_rec;
   TASK_CHECK(find_record(buf, base, x_site.id, &x_rec));
   TASK_CHECK(x_rec.tag == kGlslTraceOutput * 8 + 1);
   TASK_CHECK(x_rec.p[0] == r.color.w[0]); // only the written component
   TASK_CHECK(x_rec.p[1] == 0 && x_rec.p[2] == 0 && x_rec.p[3] == 0);

   const auto& z_site = c.info->sites[12]; // id 13, color.z
   TASK_CHECK(z_site.write_mask == 4);
   Rec z_rec;
   TASK_CHECK(find_record(buf, base, z_site.id, &z_rec));
   TASK_CHECK(z_rec.p[0] == r.color.w[2]);

   // then-path two-component store (mask 3): n=0 keeps a=3.25 so b=a-gain>0
   std::map<std::string, Value> uni2;
   uni2["gain"] = Value::from_float(2.0f);
   uni2["n"] = Value::from_int(0);
   RunResult r2 = run_traced(c, cfg, 2, 3, uni2, 0xDEADBEEFu, 10u);
   const auto& buf2 = r2.storage.at(c.info->buffer_name);
   const auto& xy_site = c.info->sites[8]; // id 9, color.xy
   TASK_CHECK(xy_site.write_mask == 3 && xy_site.components == 2);
   Rec xy;
   TASK_CHECK(find_record(buf2, base, xy_site.id, &xy));
   TASK_CHECK(xy.tag == kGlslTraceOutput * 8 + 2);
   TASK_CHECK(xy.p[0] == r2.color.w[0] && xy.p[1] == r2.color.w[1]);
   TASK_CHECK(xy.p[2] == 0 && xy.p[3] == 0);
   glslopt_shader_delete(c.shader);
   glslopt_cleanup(c.ctx);
   return 0;
}

static int
case_exec_conditional_discard()
{
   glslopt_trace_config cfg{9, 1, 1, 4, 4, 100};
   Compiled c = compile_traced(kSampleA, cfg);
   TASK_CHECK(glslopt_get_status(c.shader));

   std::map<std::string, Value> uni;
   uni["gain"] = Value::from_float(-20.0f);
   uni["n"] = Value::from_int(1);
   RunResult r = run_traced(c, cfg, 2, 3, uni, 0xDEADBEEFu, 10u);
   TASK_CHECK(r.discarded);

   size_t base = pixel_base(c.info, cfg, 2, 3);
   const auto& buf = r.storage.at(c.info->buffer_name);
   unsigned attempted = buf[base];
   TASK_CHECK(attempted >= 2);
   Rec last = read_rec(buf, base, attempted - 1);
   // discard is logged before it executes and is the final recorded event
   TASK_CHECK(last.site == 15);
   TASK_CHECK(last.tag == kGlslTraceDiscard * 8 + 0);
   TASK_CHECK(last.p[0] == 0 && last.p[1] == 0 && last.p[2] == 0 && last.p[3] == 0);
   // color.w store after the discard never ran and stays unwritten
   TASK_CHECK(r.color.w[3] == 0);
   // branch 14 evaluated true and directly precedes the discard record
   bool saw_branch14 = false;
   for (unsigned i = 0; i + 1 < attempted; i++) {
      Rec rc = read_rec(buf, base, i);
      if (rc.site == 14) { saw_branch14 = true; TASK_CHECK(rc.p[0] == 1); }
   }
   TASK_CHECK(saw_branch14);
   glslopt_shader_delete(c.shader);
   glslopt_cleanup(c.ctx);
   return 0;
}

static int
case_exec_capacity_zero()
{
   glslopt_trace_config cfg{9, 1, 1, 4, 4, 0};
   Compiled c = compile_traced(kSampleA, cfg);
   TASK_CHECK(glslopt_get_status(c.shader));
   TASK_CHECK(c.info->segment_words == 2);

   std::map<std::string, Value> uni;
   uni["gain"] = Value::from_float(2.0f);
   uni["n"] = Value::from_int(3);
   RunResult r = run_traced(c, cfg, 2, 3, uni, 0xDEADBEEFu, 10u);
   size_t base = pixel_base(c.info, cfg, 2, 3);
   const auto& buf = r.storage.at(c.info->buffer_name);
   TASK_CHECK(buf[base] == 22);
   TASK_CHECK(buf[base + 1] == 22); // every event dropped
   // no record word was ever touched
   for (unsigned w = 2; w < c.info->segment_words; w++)
      TASK_CHECK(buf[base + w] == 0xDEADBEEFu);
   // original side effects and outputs are unaffected
   TASK_CHECK(r.storage.at("orig")[0] == 13u);
   TASK_CHECK(r.color.w[3] == f2u(1.0f));
   glslopt_shader_delete(c.shader);
   glslopt_cleanup(c.ctx);
   return 0;
}

static int
case_exec_full_keeps_side_effects()
{
   glslopt_trace_config cfg{9, 1, 1, 4, 4, 3};
   Compiled c = compile_traced(kSampleA, cfg);
   TASK_CHECK(glslopt_get_status(c.shader));

   std::map<std::string, Value> uni;
   uni["gain"] = Value::from_float(2.0f);
   uni["n"] = Value::from_int(5);
   RunResult r = run_traced(c, cfg, 2, 3, uni, 0xDEADBEEFu, 10u);
   size_t base = pixel_base(c.info, cfg, 2, 3);
   const auto& buf = r.storage.at(c.info->buffer_name);
   unsigned attempted = buf[base];
   TASK_CHECK(attempted > 3);
   TASK_CHECK(buf[base + 1] == attempted - 3);

   // exactly three records, and they are the first three events
   Rec r0 = read_rec(buf, base, 0), r1 = read_rec(buf, base, 1), r2 = read_rec(buf, base, 2);
   TASK_CHECK(r0.site == 1 && r1.site == 2 && r2.site == 3);
   // later record slots were never written and keep the sentinel
   for (size_t w = base + 2 + 3 * 6; w < base + c.info->segment_words; w++)
      TASK_CHECK(buf[w] == 0xDEADBEEFu);

   // truncation never restricted the original loop or its SSBO writes
   TASK_CHECK(r.storage.at("orig")[0] == 15u);
   TASK_CHECK(!r.discarded && r.color.w[3] == f2u(1.0f));
   glslopt_shader_delete(c.shader);
   glslopt_cleanup(c.ctx);
   return 0;
}

// ---- compat group ---------------------------------------------------------

static int
case_compat_equivalence()
{
   for (int sample = 0; sample < 2; sample++) {
      const char* src = sample == 0 ? kSampleA : kSampleB;
      glslopt_trace_config cfg{9, 1, 1, 4, 4, 100};
      auto* ctx = glslopt_initialize(kGlslTargetOpenGL);
      auto* plain = glslopt_optimize(ctx, kGlslOptShaderFragment, src, 0);
      auto* traced = glslopt_optimize_traced(ctx, kGlslOptShaderFragment, src, 0, &cfg);
      TASK_CHECK(glslopt_get_status(plain) && glslopt_get_status(traced));

      std::map<std::string, Value> uni;
      if (sample == 0) {
         uni["gain"] = Value::from_float(2.0f);
         uni["n"] = Value::from_int(3);
      } else {
         Value u = Value::vec(bridge::F32, 4);
         u.w[0] = f2u(1.5f); u.w[1] = f2u(-2.25f); u.w[2] = 0x80000000u; u.w[3] = f2u(0.0f);
         uni["u"] = u;
      }
      std::map<std::string, size_t> sizes;
      sizes["orig"] = 16;
      const auto* info = glslopt_shader_get_trace_info(traced);
      sizes[info->buffer_name] = (size_t)cfg.width * cfg.height * info->segment_words;

      RunResult a = bridge::run_shader(glslopt_get_output(plain), 2, 3, uni, sizes,
                                       info->buffer_name, 0xDEADBEEFu, 10u);
      RunResult b = bridge::run_shader(glslopt_get_output(traced), 2, 3, uni, sizes,
                                       info->buffer_name, 0xDEADBEEFu, 10u);
      TASK_CHECK(a.discarded == b.discarded);
      for (int i = 0; i < 4; i++) TASK_CHECK(a.color.w[i] == b.color.w[i]);
      // sample A carries a user SSBO; sample B has none
      if (a.storage.count("orig") && b.storage.count("orig"))
         TASK_CHECK(a.storage.at("orig")[0] == b.storage.at("orig")[0]);
      glslopt_shader_delete(plain);
      glslopt_shader_delete(traced);
      glslopt_cleanup(ctx);
   }
   return 0;
}

static int
case_compat_region_clipping()
{
   glslopt_trace_config cfg{9, 1, 1, 4, 4, 100};
   Compiled c = compile_traced(kSampleA, cfg);
   TASK_CHECK(glslopt_get_status(c.shader));

   std::map<std::string, Value> uni;
   uni["gain"] = Value::from_float(2.0f);
   uni["n"] = Value::from_int(2);

   // fragment outside the region: the trace segment is never touched
   RunResult out = run_traced(c, cfg, 0, 0, uni, 0xDEADBEEFu, 10u);
   const auto& tname = c.info->buffer_name;
   const auto& obuf = out.storage.at(tname);
   for (uint32_t word : obuf) TASK_CHECK(word == 0xDEADBEEFu);
   // ... but the shader still ran normally
   TASK_CHECK(!out.discarded && out.color.w[3] == f2u(1.0f));
   TASK_CHECK(out.storage.at("orig")[0] == 12u);

   // an in-region fragment records normally and shares the same color result
   RunResult in = run_traced(c, cfg, 2, 3, uni, 0xDEADBEEFu, 10u);
   size_t base = pixel_base(c.info, cfg, 2, 3);
   TASK_CHECK(in.storage.at(tname)[base] > 0);
   for (int i = 0; i < 4; i++) TASK_CHECK(in.color.w[i] == out.color.w[i]);
   glslopt_shader_delete(c.shader);
   glslopt_cleanup(c.ctx);
   return 0;
}

static int
case_compat_names_and_lifetime()
{
   const char* conflict_src =
      "#version 430\nlayout(std430,binding=4) buffer glslopt_trace_block { uint events[]; } glslopt_trace_log;\n"
      "out vec4 color;\nvoid main(){ glslopt_trace_log.events[0]=1u; color=vec4(1.0); }\n";
   glslopt_trace_config cfg{9, 0, 0, 2, 2, 4};
   Compiled c = compile_traced(conflict_src, cfg);
   TASK_CHECK(glslopt_get_status(c.shader));
   // the generated buffer name avoids the user's variable/type names
   TASK_CHECK(std::strcmp(c.info->buffer_name, "glslopt_trace_log") != 0);
   TASK_CHECK(std::strcmp(c.info->buffer_type_name, "glslopt_trace_block") != 0);
   TASK_CHECK(c.info->binding == 9);

   // interleaved compilations with different configs, then context teardown
   auto* ctx_a = glslopt_initialize(kGlslTargetOpenGL);
   auto* ctx_b = glslopt_initialize(kGlslTargetOpenGL);
   glslopt_trace_config cfg1{3, 0, 0, 2, 2, 5}, cfg2{6, 0, 0, 3, 3, 7};
   auto* s1 = glslopt_optimize_traced(ctx_a, kGlslOptShaderFragment, kSampleB, 0, &cfg1);
   auto* s2 = glslopt_optimize_traced(ctx_b, kGlslOptShaderFragment, kSampleB, 0, &cfg2);
   TASK_CHECK(glslopt_get_status(s1) && glslopt_get_status(s2));
   const auto* i1 = glslopt_shader_get_trace_info(s1);
   const auto* i2 = glslopt_shader_get_trace_info(s2);
   TASK_CHECK(i1->binding == 3 && i2->binding == 6);
   TASK_CHECK(i1->segment_words == 2 + 6 * 5 && i2->segment_words == 2 + 6 * 7);

   // results stay readable after their producing context is gone
   std::string out1 = glslopt_get_output(s1);
   glslopt_cleanup(ctx_a);
   TASK_CHECK(glslopt_get_status(s1));
   TASK_CHECK(std::strcmp(glslopt_get_output(s1), out1.c_str()) == 0);
   TASK_CHECK(glslopt_shader_get_trace_info(s1)->site_count == i1->site_count);
   TASK_CHECK(glslopt_shader_get_trace_info(s2)->binding == 6);
   glslopt_shader_delete(s1);
   glslopt_shader_delete(s2);
   glslopt_cleanup(ctx_b);
   glslopt_shader_delete(c.shader);
   glslopt_cleanup(c.ctx);
   return 0;
}

const task_case task_cases[] = {
   {"catalog", "contents", case_catalog_contents},
   {"catalog", "reflection", case_catalog_reflection},
   {"catalog", "failures", case_catalog_failures},
   {"execution", "float_payload", case_exec_float_payload},
   {"execution", "branch_loop_order", case_exec_branch_loop_order},
   {"execution", "partial_output", case_exec_partial_output},
   {"execution", "conditional_discard", case_exec_conditional_discard},
   {"execution", "capacity_zero", case_exec_capacity_zero},
   {"execution", "full_keeps_side_effects", case_exec_full_keeps_side_effects},
   {"compat", "equivalence", case_compat_equivalence},
   {"compat", "region_clipping", case_compat_region_clipping},
   {"compat", "names_and_lifetime", case_compat_names_and_lifetime},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
