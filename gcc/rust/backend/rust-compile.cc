#include "rust-compile.h"
#include "rust-diagnostics.h"

namespace Rust {
namespace Compile {

#define VISIT_POP(L, S, R, B)                                                  \
  do                                                                           \
    {                                                                          \
      auto before = B.size ();                                                 \
      S->accept_vis (*this);                                                   \
      if (B.size () <= before)                                                 \
	break;                                                                 \
      R = B.back ();                                                           \
      B.pop_back ();                                                           \
    }                                                                          \
  while (0)

Compilation::Compilation (AST::Crate &crate, ::Backend *backend)
  : crate (crate), backend (backend), scope (backend)
{}

Compilation::~Compilation () {}

bool
Compilation::Compile (AST::Crate &crate, ::Backend *backend)
{
  Compilation resolver (crate, backend);
  return resolver.go ();
}

bool
Compilation::go ()
{
  scope.Push ();

  // builtin primitives
  scope.InsertType ("bool",
		    backend->named_type ("bool", backend->bool_type (),
					 Linemap::predeclared_location ()));
  scope.InsertType ("i64",
		    backend->named_type ("i64",
					 backend->integer_type (false, 64),
					 Linemap::predeclared_location ()));
  scope.InsertType ("i32",
		    backend->named_type ("i32",
					 backend->integer_type (false, 32),
					 Linemap::predeclared_location ()));
  scope.InsertType ("i16",
		    backend->named_type ("i16",
					 backend->integer_type (false, 16),
					 Linemap::predeclared_location ()));
  scope.InsertType ("i8",
		    backend->named_type ("i8", backend->integer_type (false, 8),
					 Linemap::predeclared_location ()));
  scope.InsertType ("u64",
		    backend->named_type ("u64",
					 backend->integer_type (true, 64),
					 Linemap::predeclared_location ()));
  scope.InsertType ("u32",
		    backend->named_type ("u32",
					 backend->integer_type (true, 32),
					 Linemap::predeclared_location ()));
  scope.InsertType ("u16",
		    backend->named_type ("u16",
					 backend->integer_type (true, 16),
					 Linemap::predeclared_location ()));
  scope.InsertType ("u8",
		    backend->named_type ("u8", backend->integer_type (true, 8),
					 Linemap::predeclared_location ()));
  scope.InsertType ("f64", backend->float_type (64));
  scope.InsertType ("f32", backend->float_type (32));

  for (auto &item : crate.items)
    item->accept_vis (*this);
  scope.Pop ();

  // Define all globally declared values.
  if (saw_errors ())
    return false;

  backend->write_global_definitions (type_decls, const_decls, func_decls,
				     var_decls);
  return true;
}

bool
Compilation::compileVarDecl (Bfunction *fndecl, AST::LetStmt *stmt,
			     std::vector<Bvariable *> &vars)
{
  AST::Type *type = stmt->has_type () ? stmt->get_type ().get () : stmt->inferedType;
  translatedType = NULL;
  type->accept_vis (*this);
  if (translatedType == NULL)
    {
      rust_error_at (stmt->get_locus (), "failed to compile type for var decl");
      return false;
    }

  stmt->get_pattern ()->accept_vis (*this);
  for (auto &pattern : patternBuffer)
    {
      auto var = backend->local_variable (fndecl, pattern.get_ident (),
					  translatedType, NULL /*decl_var*/,
					  false /*address_taken*/, stmt->get_locus ());
      vars.push_back (var);
      scope.InsertVar (pattern.get_ident (), var);
    }
  patternBuffer.clear ();
  return true;
}

Bexpression *
Compilation::compileBooleanLiteral (std::string val)
{
  bool bval = val.compare ("true") == 0;
  return backend->boolean_constant_expression (bval);
}

Bexpression *
Compilation::compileFloatLiteral (std::string val, Location locus)
{
  Btype *type = NULL;
  bool ok = scope.LookupType ("f32", &type);
  if (!ok)
    {
      rust_fatal_error (locus, "unable to find type");
      return NULL;
    }
  mpfr_t fval;
  if (mpfr_init_set_str (fval, val.c_str (), 10, GMP_RNDN) != 0)
    {
      rust_fatal_error (locus, "bad number in literal");
      return NULL;
    }
  return backend->float_constant_expression (type, fval);
}

Bexpression *
Compilation::compileIntegerLiteral (std::string val, Location locus)
{
  Btype *type = NULL;
  bool ok = scope.LookupType ("i32", &type);
  if (!ok)
    {
      rust_fatal_error (locus, "unable to find type");
      return NULL;
    }
  mpz_t ival;
  if (mpz_init_set_str (ival, val.c_str (), 10) != 0)
    {
      rust_fatal_error (locus, "bad number in literal");
      return NULL;
    }
  return backend->integer_constant_expression (type, ival);
}

void
Compilation::visit (AST::Token &tok)
{}

void
Compilation::visit (AST::DelimTokenTree &delim_tok_tree)
{}

void
Compilation::visit (AST::AttrInputMetaItemContainer &input)
{}

void
Compilation::visit (AST::IdentifierExpr &ident_expr)
{
  Bvariable *var = NULL;
  if (!scope.LookupVar (ident_expr.as_string (), &var))
    {
      rust_fatal_error (ident_expr.get_locus (), "unknown var");
      return;
    }
  exprs.push_back (backend->var_expression (var, ident_expr.get_locus ()));
}

void
Compilation::visit (AST::Lifetime &lifetime)
{}

void
Compilation::visit (AST::LifetimeParam &lifetime_param)
{}

void
Compilation::visit (AST::MacroInvocationSemi &macro)
{}

// rust-path.h
void
Compilation::visit (AST::PathInExpression &path)
{
  Bfunction *fn = NULL;
  if (scope.LookupFunction (path.as_string (), &fn))
    {
      auto expr
	= backend->function_code_expression (fn, path.get_locus ());
      exprs.push_back (expr);
      translatedType = scope.GetFnRetType (fn);
      return;
    }
}

void
Compilation::visit (AST::TypePathSegment &segment)
{}
void
Compilation::visit (AST::TypePathSegmentGeneric &segment)
{}
void
Compilation::visit (AST::TypePathSegmentFunction &segment)
{}

void
Compilation::visit (AST::TypePath &path)
{
  if (path.get_segments ().size () > 1)
    {
      rust_error_at (path.get_locus (), "unable to compile multi segment types yet");
      return;
    }

  Btype *type = NULL;
  if (!scope.LookupType (path.as_string (), &type))
    {
      rust_error_at (path.get_locus (), "unknown type");
      return;
    }
  translatedType = type;
}

void
Compilation::visit (AST::QualifiedPathInExpression &path)
{}
void
Compilation::visit (AST::QualifiedPathInType &path)
{}

// rust-expr.h
void
Compilation::visit (AST::LiteralExpr &expr)
{
  Bexpression *compiled;
  switch (expr.get_lit_type ())
    {
    case AST::Literal::BOOL:
      compiled = compileBooleanLiteral (expr.as_string ());
      break;

    case AST::Literal::FLOAT:
      compiled
	= compileFloatLiteral (expr.as_string (), expr.get_locus ());
      break;

    case AST::Literal::INT:
      compiled
	= compileIntegerLiteral (expr.as_string (), expr.get_locus ());
      break;

    default:
      rust_fatal_error (expr.get_locus (), "unknown literal");
      return;
    }

  exprs.push_back (compiled);
}

void
Compilation::visit (AST::AttrInputLiteral &attr_input)
{}
void
Compilation::visit (AST::MetaItemLitExpr &meta_item)
{}
void
Compilation::visit (AST::MetaItemPathLit &meta_item)
{}
void
Compilation::visit (AST::BorrowExpr &expr)
{}
void
Compilation::visit (AST::DereferenceExpr &expr)
{}
void
Compilation::visit (AST::ErrorPropagationExpr &expr)
{}

void
Compilation::visit (AST::NegationExpr &expr)
{
  Bexpression *root = NULL;
  VISIT_POP (expr.get_negated_expr ()->get_locus_slow (), expr.get_negated_expr ().get (), root,
	     exprs);
  if (root == NULL)
    {
      rust_error_at (expr.get_negated_expr ()->get_locus_slow (), "failed to compile");
      return;
    }

  Operator op;
  switch (expr.get_negation_type ())
    {
    case AST::NegationExpr::NEGATE:
      op = OPERATOR_MINUS;
      break;
    case AST::NegationExpr::NOT:
      op = OPERATOR_NOT;
      break;
    default:
      rust_fatal_error (expr.get_locus (), "failed to compile operator");
      return;
    }

  auto unary = backend->unary_expression (op, root, expr.get_locus ());
  exprs.push_back (unary);
}

void
Compilation::visit (AST::ArithmeticOrLogicalExpr &expr)
{
  Bexpression *lhs = NULL;
  VISIT_POP (expr.get_left_expr ()->get_locus_slow (), expr.get_left_expr ().get (), lhs, exprs);
  if (lhs == NULL)
    {
      rust_error_at (expr.get_left_expr ()->get_locus_slow (), "failed to compile");
      return;
    }

  Bexpression *rhs = NULL;
  VISIT_POP (expr.get_right_expr ()->get_locus_slow (), expr.get_right_expr ().get (), rhs, exprs);
  if (rhs == NULL)
    {
      rust_error_at (expr.get_right_expr ()->get_locus_slow (), "failed to compile");
      return;
    }

  Operator op;
  switch (expr.get_expr_type ())
    {
    case AST::ArithmeticOrLogicalExpr::ADD:
      op = OPERATOR_PLUS;
      break;
    case AST::ArithmeticOrLogicalExpr::SUBTRACT:
      op = OPERATOR_MINUS;
      break;
    case AST::ArithmeticOrLogicalExpr::MULTIPLY:
      op = OPERATOR_MULT;
      break;
    case AST::ArithmeticOrLogicalExpr::DIVIDE:
      op = OPERATOR_DIV;
      break;
    case AST::ArithmeticOrLogicalExpr::MODULUS:
      op = OPERATOR_MOD;
      break;
    case AST::ArithmeticOrLogicalExpr::BITWISE_AND:
      op = OPERATOR_AND;
      break;
    case AST::ArithmeticOrLogicalExpr::BITWISE_OR:
      op = OPERATOR_OR;
      break;
    case AST::ArithmeticOrLogicalExpr::BITWISE_XOR:
      op = OPERATOR_XOR;
      break;
    case AST::ArithmeticOrLogicalExpr::LEFT_SHIFT:
      op = OPERATOR_LSHIFT;
      break;
    case AST::ArithmeticOrLogicalExpr::RIGHT_SHIFT:
      op = OPERATOR_RSHIFT;
      break;
    default:
      rust_fatal_error (expr.get_locus (), "failed to compile operator");
      return;
    }

  auto binExpr
    = backend->binary_expression (op, lhs, rhs, expr.get_locus ());
  exprs.push_back (binExpr);
}

void
Compilation::visit (AST::ComparisonExpr &expr)
{
  Bexpression *lhs = NULL;
  VISIT_POP (expr.get_left_expr ()->get_locus_slow (), expr.get_left_expr ().get (), lhs, exprs);
  if (lhs == NULL)
    {
      rust_error_at (expr.get_left_expr ()->get_locus_slow (), "failed to compile");
      return;
    }

  Bexpression *rhs = NULL;
  VISIT_POP (expr.get_right_expr ()->get_locus_slow (), expr.get_right_expr ().get (), rhs, exprs);
  if (rhs == NULL)
    {
      rust_error_at (expr.get_right_expr ()->get_locus_slow (), "failed to compile");
      return;
    }

  Operator op;
  switch (expr.get_expr_type ())
    {
    case AST::ComparisonExpr::EQUAL:
      op = OPERATOR_EQEQ;
      break;
    case AST::ComparisonExpr::NOT_EQUAL:
      op = OPERATOR_NOTEQ;
      break;
    case AST::ComparisonExpr::GREATER_THAN:
      op = OPERATOR_GT;
      break;
    case AST::ComparisonExpr::LESS_THAN:
      op = OPERATOR_LT;
      break;
    case AST::ComparisonExpr::GREATER_OR_EQUAL:
      op = OPERATOR_GE;
      break;
    case AST::ComparisonExpr::LESS_OR_EQUAL:
      op = OPERATOR_LE;
      break;
    default:
      rust_fatal_error (expr.get_locus (), "failed to compile operator");
      return;
    }

  auto compExpr
    = backend->binary_expression (op, lhs, rhs, expr.get_locus ());
  exprs.push_back (compExpr);
}

void
Compilation::visit (AST::LazyBooleanExpr &expr)
{
  Bexpression *lhs = NULL;
  VISIT_POP (expr.get_left_expr ()->get_locus_slow (), expr.get_left_expr ().get (), lhs, exprs);
  if (lhs == NULL)
    {
      rust_error_at (expr.get_left_expr ()->get_locus_slow (), "failed to compile");
      return;
    }

  Bexpression *rhs = NULL;
  VISIT_POP (expr.get_right_expr ()->get_locus_slow (), expr.get_right_expr ().get (), rhs, exprs);
  if (rhs == NULL)
    {
      rust_error_at (expr.get_right_expr ()->get_locus_slow (), "failed to compile");
      return;
    }

  Operator op;
  switch (expr.get_expr_type ())
    {
    case AST::LazyBooleanExpr::LOGICAL_OR:
      op = OPERATOR_OROR;
      break;
    case AST::LazyBooleanExpr::LOGICAL_AND:
      op = OPERATOR_ANDAND;
      break;
    default:
      rust_fatal_error (expr.get_locus (), "failed to compile operator");
      return;
    }

  auto compExpr
    = backend->binary_expression (op, lhs, rhs, expr.get_locus ());
  exprs.push_back (compExpr);
}

void
Compilation::visit (AST::TypeCastExpr &expr)
{}

void
Compilation::visit (AST::AssignmentExpr &expr)
{
  Bexpression *lhs = NULL;
  VISIT_POP (expr.get_left_expr ()->get_locus_slow (), expr.get_left_expr ().get (), lhs, exprs);
  if (lhs == NULL)
    {
      rust_error_at (expr.get_left_expr ()->get_locus_slow (), "failed to compile");
      return;
    }

  Bexpression *rhs = NULL;
  VISIT_POP (expr.get_right_expr ()->get_locus_slow (), expr.get_right_expr ().get (), rhs, exprs);
  if (rhs == NULL)
    {
      rust_error_at (expr.get_right_expr ()->get_locus_slow (), "failed to compile");
      return;
    }

  auto s = backend->assignment_statement (scope.GetCurrentFndecl (), lhs, rhs,
					  expr.get_locus ());
  scope.AddStatement (s);
}

void
Compilation::visit (AST::CompoundAssignmentExpr &expr)
{}
void
Compilation::visit (AST::GroupedExpr &expr)
{}

void
Compilation::visit (AST::ArrayElemsValues &elems)
{
  std::vector< ::Bexpression *> elements;

  bool failed = false;
  elems.iterate ([&] (AST::Expr *expr) mutable -> bool {
    Bexpression *value = nullptr;
    VISIT_POP (expr.get_locus_slow (), expr, value, exprs);
    if (value == nullptr)
      {
	rust_fatal_error (expr->get_locus_slow (),
			  "failed to compile value to array initialiser");
	return false;
      }
    elements.push_back (value);
    return true;
  });

  // nothing to do when its failed
  if (failed)
    return;

  arrayConsStack.push_back (elements);
}

void
Compilation::visit (AST::ArrayElemsCopied &elems)
{}

void
Compilation::visit (AST::ArrayExpr &expr)
{
  translatedType = nullptr;
  expr.get_inferred_type ()->accept_vis (*this);
  if (translatedType == nullptr)
    {
      rust_error_at (expr.get_locus_slow (),
		     "failed to compile array type for ArrayExpr");
      return;
    }

  ::Btype *compiledType = translatedType;
  translatedType = nullptr;

  auto before = arrayConsStack.size ();
  expr.get_array_elems ()->accept_vis (*this);
  if (arrayConsStack.size () <= before)
    {
      rust_error_at (expr.get_locus_slow (),
		     "failed to compile the array constructor");
      return;
    }
  std::vector< ::Bexpression *> initializer = arrayConsStack.back ();
  arrayConsStack.pop_back ();

  std::vector<unsigned long> indexes;
  for (unsigned long i = 0; i < initializer.size (); ++i)
    indexes.push_back (i);

  Bexpression *cons
    = backend->array_constructor_expression (compiledType, indexes, initializer,
					     expr.get_locus_slow ());
  exprs.push_back (cons);
}

void
Compilation::visit (AST::ArrayIndexExpr &expr)
{
  Bexpression *arrayExpr = nullptr;
  VISIT_POP (expr.get_array_expr ()->get_locus_slow (), expr.get_array_expr (),
	     arrayExpr, exprs);
  if (arrayExpr == nullptr)
    {
      rust_error_at (expr.get_locus_slow (),
		     "failed to compile value to array expression reference");
      return;
    }

  Bexpression *indexExpr = nullptr;
  VISIT_POP (expr.get_index_expr ()->get_locus_slow (), expr.get_index_expr (),
	     indexExpr, exprs);
  if (indexExpr == nullptr)
    {
      rust_error_at (expr.get_locus_slow (),
		     "failed to compile value to array index expression");
      return;
    }

  Bexpression *indexExpression
    = backend->array_index_expression (arrayExpr, indexExpr,
				       expr.get_locus_slow ());
  exprs.push_back (indexExpression);
}

void
Compilation::visit (AST::TupleExpr &expr)
{}
void
Compilation::visit (AST::TupleIndexExpr &expr)
{}
void
Compilation::visit (AST::StructExprStruct &expr)
{}
// void Compilation::visit(StructExprField& field) {}
void
Compilation::visit (AST::StructExprFieldIdentifier &field)
{}

void
Compilation::visit (AST::StructExprFieldIdentifierValue &field)
{
  Bexpression *value = NULL;
  VISIT_POP (field.get_value ()->get_locus_slow (), field.get_value ().get (), value, exprs);
  if (value == NULL)
    {
      rust_fatal_error (field.get_value ()->get_locus_slow (),
			"failed to compile value to struct");
      return;
    }
  exprs.push_back (value);
}

void
Compilation::visit (AST::StructExprFieldIndexValue &field)
{
  Bexpression *value = NULL;
  VISIT_POP (field.get_value ()->get_locus_slow (), field.get_value ().get (), value, exprs);
  if (value == NULL)
    {
      rust_fatal_error (field.get_value ()->get_locus_slow (),
			"failed to compile value to struct");
      return;
    }
  exprs.push_back (value);
}

void
Compilation::visit (AST::StructExprStructFields &expr)
{
  AST::StructStruct *decl = NULL;
  if (!scope.LookupStructDecl (expr.get_struct_name ().as_string (), &decl))
    {
      rust_error_at (expr.get_locus (), "unknown type");
      return;
    }

  Btype *structType = NULL;
  if (!scope.LookupType (expr.get_struct_name ().as_string (), &structType))
    {
      rust_fatal_error (expr.get_locus (), "unknown type");
      return;
    }

  structBuffer.push_back (decl);
  std::vector<Bexpression *> constructor;

  // FIXME type resolution pass should ensures these are in correct order
  // and have defaults if required
  for (auto &field : expr.get_fields ())
    {
      Bexpression *value = NULL;
      VISIT_POP (expr.get_locus (), field, value, exprs);
      if (value == NULL)
	{
	  rust_fatal_error (expr.get_locus (),
			    "failed to compile value to struct");
	  return;
	}

      constructor.push_back (value);
    }

  structBuffer.pop_back ();
  auto cons = backend->constructor_expression (structType, constructor,
					       expr.get_locus ());
  exprs.push_back (cons);
}

void
Compilation::visit (AST::StructExprStructBase &expr)
{}
void
Compilation::visit (AST::StructExprTuple &expr)
{}
void
Compilation::visit (AST::StructExprUnit &expr)
{}
// void Compilation::visit(EnumExprField& field) {}
void
Compilation::visit (AST::EnumExprFieldIdentifier &field)
{}
void
Compilation::visit (AST::EnumExprFieldIdentifierValue &field)
{}
void
Compilation::visit (AST::EnumExprFieldIndexValue &field)
{}
void
Compilation::visit (AST::EnumExprStruct &expr)
{}
void
Compilation::visit (AST::EnumExprTuple &expr)
{}
void
Compilation::visit (AST::EnumExprFieldless &expr)
{}

void
Compilation::visit (AST::CallExpr &expr)
{
  Bexpression *fn = NULL;
  VISIT_POP (expr.get_function_expr ()->get_locus_slow (), expr.get_function_expr (), fn, exprs);
  if (fn == NULL)
    {
      rust_error_at (expr.get_function_expr ()->get_locus_slow (), "failed to resolve");
      return;
    }

  std::vector<Bexpression *> args;
  for (auto &param : expr.get_params ())
    {
      Bexpression *arg = NULL;
      VISIT_POP (param->get_locus_slow (), param, arg, exprs);
      if (arg == NULL)
	{
	  rust_error_at (param->get_locus_slow (),
			 "failed to compile argument");
	  return;
	}

      args.push_back (arg);
    }

  auto call = backend->call_expression (scope.GetCurrentFndecl (), fn, args,
					NULL, expr.get_locus ());
  exprs.push_back (call);
}

void
Compilation::visit (AST::MethodCallExpr &expr)
{}
void
Compilation::visit (AST::FieldAccessExpr &expr)
{}
void
Compilation::visit (AST::ClosureExprInner &expr)
{}

void
Compilation::visit (AST::BlockExpr &expr)
{
  Bblock *enclosingScope = NULL;
  Location start_location; /* = stmt.locus; FIXME */
  Location end_location;   // FIXME

  std::vector<Bvariable *> vars;
  auto code_block
    = backend->block (scope.GetCurrentFndecl (), scope.CurBlock (), vars,
		      start_location, end_location);

  scope.PushBlock (code_block);
  for (auto &stmt : expr.get_statements ())
    {
      stmt->accept_vis (*this);
    }
  // dont pop
}

void
Compilation::visit (AST::ClosureExprInnerTyped &expr)
{}
void
Compilation::visit (AST::ContinueExpr &expr)
{}
void
Compilation::visit (AST::BreakExpr &expr)
{}
void
Compilation::visit (AST::RangeFromToExpr &expr)
{}
void
Compilation::visit (AST::RangeFromExpr &expr)
{}
void
Compilation::visit (AST::RangeToExpr &expr)
{}
void
Compilation::visit (AST::RangeFullExpr &expr)
{}
void
Compilation::visit (AST::RangeFromToInclExpr &expr)
{}
void
Compilation::visit (AST::RangeToInclExpr &expr)
{}

void
Compilation::visit (AST::ReturnExpr &expr)
{
  Bexpression *ret = NULL;
  VISIT_POP (expr.get_returned_expr ()->get_locus_slow (), expr.get_returned_expr ().get (), ret, exprs);
  if (ret == NULL)
    {
      rust_fatal_error (expr.get_returned_expr ()->get_locus_slow (),
			"failed to compile");
      return;
    }

  std::vector<Bexpression *> retstmts;
  retstmts.push_back (ret);
  auto s = backend->return_statement (scope.GetCurrentFndecl (), retstmts,
				      expr.get_locus ());
  scope.AddStatement (s);
}

void
Compilation::visit (AST::UnsafeBlockExpr &expr)
{}

void
Compilation::visit (AST::LoopExpr &expr)
{}

void
Compilation::visit (AST::WhileLoopExpr &expr)
{}

void
Compilation::visit (AST::WhileLetLoopExpr &expr)
{}
void
Compilation::visit (AST::ForLoopExpr &expr)
{}

void
Compilation::visit (AST::IfExpr &expr)
{
  Bexpression *cond = NULL;
  VISIT_POP (expr.get_condition_expr ()->get_locus_slow (),
	     expr.get_condition_expr ().get (), cond, exprs);
  if (cond == NULL)
    {
      rust_error_at (expr.get_condition_expr ()->get_locus_slow (),
		     "failed to compile");
      return;
    }

  expr.vis_if_block (*this);
  Bblock *then_block = scope.PopBlock ();

  auto stmt = backend->if_statement (scope.GetCurrentFndecl (), cond,
				     then_block, NULL, expr.get_locus ());
  stmts.push_back (stmt);
}

void
Compilation::visit (AST::IfExprConseqElse &expr)
{
  Bexpression *cond = NULL;
  VISIT_POP (expr.get_condition_expr ()->get_locus_slow (),
	     expr.get_condition_expr ().get (), cond, exprs);
  if (cond == NULL)
    {
      rust_error_at (expr.get_condition_expr ()->get_locus_slow (),
		     "failed to compile");
      return;
    }

  expr.vis_if_block (*this);
  Bblock *then_block = scope.PopBlock ();

  expr.vis_else_block (*this);
  Bblock *else_block = scope.PopBlock ();

  auto stmt
    = backend->if_statement (scope.GetCurrentFndecl (), cond, then_block,
			     else_block, expr.get_locus ());
  stmts.push_back (stmt);
}

void
Compilation::visit (AST::IfExprConseqIf &expr)
{
  Bexpression *cond = NULL;
  VISIT_POP (expr.get_condition_expr ()->get_locus_slow (),
	     expr.get_condition_expr ().get (), cond, exprs);
  if (cond == NULL)
    {
      rust_error_at (expr.get_condition_expr ()->get_locus_slow (),
		     "failed to compile");
      return;
    }

  expr.vis_if_block (*this);
  Bblock *then_block = scope.PopBlock ();

  // setup else block
  Bblock *enclosingScope = NULL;
  Location start_location; /* = stmt.locus; FIXME */
  Location end_location;   // FIXME

  std::vector<Bvariable *> vars;
  auto else_block
    = backend->block (scope.GetCurrentFndecl (), scope.CurBlock (), vars,
		      start_location, end_location);

  scope.PushBlock (else_block);
  expr.vis_conseq_if_expr (*this);
  // get trailing if required
  for (auto &s : stmts)
    scope.AddStatement (s);
  stmts.clear ();
  scope.PopBlock ();

  auto stmt
    = backend->if_statement (scope.GetCurrentFndecl (), cond, then_block,
			     else_block, expr.get_locus ());
  stmts.push_back (stmt);
}

void
Compilation::visit (AST::IfExprConseqIfLet &expr)
{
  printf ("IfExprConseqIfLet %s\n", expr.as_string ().c_str ());
}
void
Compilation::visit (AST::IfLetExpr &expr)
{
  printf ("IfLetExpr %s\n", expr.as_string ().c_str ());
}
void
Compilation::visit (AST::IfLetExprConseqElse &expr)
{
  printf ("IfLetExprConseqElse %s\n", expr.as_string ().c_str ());
}

void
Compilation::visit (AST::IfLetExprConseqIf &expr)
{
  printf ("IfLetExprConseqIf %s\n", expr.as_string ().c_str ());
}

void
Compilation::visit (AST::IfLetExprConseqIfLet &expr)
{
  printf ("IfLetExprConseqIfLet %s\n", expr.as_string ().c_str ());
}

// void Compilation::visit(MatchCase& match_case) {}
/*void
Compilation::visit (AST::MatchCaseBlockExpr &match_case)
{}*/
/*void
Compilation::visit (AST::MatchCaseExpr &match_case)
{}*/
void
Compilation::visit (AST::MatchExpr &expr)
{}
void
Compilation::visit (AST::AwaitExpr &expr)
{}
void
Compilation::visit (AST::AsyncBlockExpr &expr)
{}

// rust-item.h
void
Compilation::visit (AST::TypeParam &param)
{}
// void Compilation::visit(WhereClauseItem& item) {}
void
Compilation::visit (AST::LifetimeWhereClauseItem &item)
{}
void
Compilation::visit (AST::TypeBoundWhereClauseItem &item)
{}
void
Compilation::visit (AST::Method &method)
{}
void
Compilation::visit (AST::ModuleBodied &module)
{}
void
Compilation::visit (AST::ModuleNoBody &module)
{}
void
Compilation::visit (AST::ExternCrate &crate)
{}
// void Compilation::visit(UseTree& use_tree) {}
void
Compilation::visit (AST::UseTreeGlob &use_tree)
{}
void
Compilation::visit (AST::UseTreeList &use_tree)
{}
void
Compilation::visit (AST::UseTreeRebind &use_tree)
{}
void
Compilation::visit (AST::UseDeclaration &use_decl)
{}

void
Compilation::visit (AST::Function &function)
{
  Backend::Btyped_identifier receiver;
  std::vector<Backend::Btyped_identifier> parameters;
  std::vector<Backend::Btyped_identifier> results;

  for (auto &param : function.get_function_params ())
    {
      // translate the type
      translatedType = NULL;
      param.get_type ()->accept_vis (*this);
      if (translatedType == NULL)
	{
	  rust_error_at (param.get_locus (), "failed to generate type for parameter");
	  return;
	}

      auto before = patternBuffer.size ();
      param.get_pattern ()->accept_vis (*this);
      if (patternBuffer.size () <= before)
	{
	  rust_error_at (param.get_locus (), "failed to analyse parameter name");
	  return;
	}

      auto numParamsPerType = patternBuffer.size () - before;
      for (size_t i = 0; i < numParamsPerType; i++)
	{
	  auto paramName = patternBuffer.back ();
	  patternBuffer.pop_back ();
	  parameters.push_back (
	    Backend::Btyped_identifier (paramName.get_ident (),
					translatedType, param.get_locus ()));
	}
    }

  Btype *returnType = NULL;
  if (function.has_return_type ())
    {
      translatedType = NULL;
      function.get_return_type ()->accept_vis (*this);
      if (translatedType == NULL)
	{
	  rust_fatal_error (function.get_locus (),
			    "failed to generate type for function");
	  return;
	}
      returnType = translatedType;

      // add into the results:
      results.push_back (
	Backend::Btyped_identifier ("_", translatedType, Location ()));
    }

  Btype *fntype = backend->function_type (receiver, parameters, results, NULL,
					  function.get_locus ());
  Bfunction *fndecl
    = backend->function (fntype, function.get_function_name (), "" /* asm_name */,
			 0 /* flags */, function.get_locus ());

  scope.InsertFunction (function.get_function_name (), fndecl, returnType);
  scope.Push ();

  // setup the params
  std::vector<Bvariable *> param_vars;
  for (auto &param : parameters)
    {
      bool tree_addressable = false;
      auto p = backend->parameter_variable (fndecl, param.name, param.btype,
					    tree_addressable, param.location);

      scope.InsertVar (param.name, p);
      param_vars.push_back (p);
    }

  if (!backend->function_set_parameters (fndecl, param_vars))
    {
      rust_error_at (function.get_locus (), "failed to setup parameter variables");
      return;
    }

  std::vector<Bvariable *> vars;
  for (auto &decl : function.locals)
    {
      if (!compileVarDecl (fndecl, decl, vars))
	{
	  rust_error_at (decl->get_locus (), "failed to compile var decl");
	  return;
	}
    }

  // is null for top level functions - nested functions will have an enclosing
  // scope
  Bblock *enclosingScope = NULL;
  Location start_location = function.get_locus ();
  Location end_location;
  if (function.get_definition ()->get_statements ().size () > 0)
    {
      end_location
	= function.get_definition ()->get_statements ().back ()->get_locus_slow ();
    }

  auto code_block = backend->block (fndecl, enclosingScope, vars,
				    start_location, end_location);

  scope.PushBlock (code_block);

  Bvariable *retDecl = NULL;
  if (function.has_return_type ())
    {
      bool address_is_taken = false;
      Bstatement *ret_var_stmt = NULL;
      retDecl = backend->temporary_variable (fndecl, code_block, returnType,
					     NULL, address_is_taken,
					     function.get_locus (), &ret_var_stmt);
      scope.AddStatement (ret_var_stmt);
    }
  scope.PushCurrentFunction (function.get_function_name (), fndecl, returnType,
			     retDecl);

  for (auto &stmt : function.get_definition ()->get_statements ())
    stmt->accept_vis (*this);

  scope.PopBlock ();

  auto body = backend->block_statement (code_block);
  if (!backend->function_set_body (fndecl, body))
    {
      rust_error_at (function.get_locus (), "failed to set body to function");
      return;
    }

  scope.Pop ();
  scope.PopCurrentFunction ();

  func_decls.push_back (fndecl);
}

void
Compilation::visit (AST::TypeAlias &type_alias)
{}

void
Compilation::visit (AST::StructStruct &struct_item)
{
  std::vector<Backend::Btyped_identifier> fields;
  for (auto &field : struct_item.get_fields ())
    {
      translatedType = NULL;
      field.get_field_type ()->accept_vis (*this);
      if (translatedType == NULL)
	{
	  rust_fatal_error (
	    struct_item.get_locus () /* StructField is mi sing locus */,
	    "failed to compile struct field");
	  return;
	}

      fields.push_back (Backend::Btyped_identifier (
	field.get_field_name (), translatedType,
	struct_item.get_locus () /* StructField is mi sing locus */));
    }

  auto compiledStruct
    = backend->placeholder_struct_type (struct_item.get_struct_name (),
					struct_item.get_locus ());
  bool ok = backend->set_placeholder_struct_type (compiledStruct, fields);
  if (!ok)
    {
      rust_fatal_error (struct_item.get_locus (), "failed to compile struct");
      return;
    }

  type_decls.push_back (compiledStruct);
  scope.InsertType (struct_item.get_struct_name (), compiledStruct);
  scope.InsertStructDecl (struct_item.get_struct_name (), &struct_item);
}

void
Compilation::visit (AST::TupleStruct &tuple_struct)
{}
void
Compilation::visit (AST::EnumItem &item)
{}
void
Compilation::visit (AST::EnumItemTuple &item)
{}
void
Compilation::visit (AST::EnumItemStruct &item)
{}
void
Compilation::visit (AST::EnumItemDiscriminant &item)
{}
void
Compilation::visit (AST::Enum &enum_item)
{}
void
Compilation::visit (AST::Union &union_item)
{}
void
Compilation::visit (AST::ConstantItem &const_item)
{}
void
Compilation::visit (AST::StaticItem &static_item)
{}
void
Compilation::visit (AST::TraitItemFunc &item)
{}
void
Compilation::visit (AST::TraitItemMethod &item)
{}
void
Compilation::visit (AST::TraitItemConst &item)
{}
void
Compilation::visit (AST::TraitItemType &item)
{}
void
Compilation::visit (AST::Trait &trait)
{}
void
Compilation::visit (AST::InherentImpl &impl)
{}
void
Compilation::visit (AST::TraitImpl &impl)
{}
// void Compilation::visit(ExternalItem& item) {}
void
Compilation::visit (AST::ExternalStaticItem &item)
{}
void
Compilation::visit (AST::ExternalFunctionItem &item)
{}
void
Compilation::visit (AST::ExternBlock &block)
{}

// rust-macro.h
void
Compilation::visit (AST::MacroMatchFragment &match)
{}
void
Compilation::visit (AST::MacroMatchRepetition &match)
{}
void
Compilation::visit (AST::MacroMatcher &matcher)
{}
void
Compilation::visit (AST::MacroRulesDefinition &rules_def)
{}
void
Compilation::visit (AST::MacroInvocation &macro_invoc)
{}
void
Compilation::visit (AST::MetaItemPath &meta_item)
{}
void
Compilation::visit (AST::MetaItemSeq &meta_item)
{}
void
Compilation::visit (AST::MetaWord &meta_item)
{}
void
Compilation::visit (AST::MetaNameValueStr &meta_item)
{}
void
Compilation::visit (AST::MetaListPaths &meta_item)
{}
void
Compilation::visit (AST::MetaListNameValueStr &meta_item)
{}

// rust-pattern.h
void
Compilation::visit (AST::LiteralPattern &pattern)
{
  printf ("LiteralPattern: %s\n", pattern.as_string ().c_str ());
}

void
Compilation::visit (AST::IdentifierPattern &pattern)
{
  patternBuffer.push_back (pattern);
}

void
Compilation::visit (AST::WildcardPattern &pattern)
{}
// void Compilation::visit(RangePatternBound& bound) {}
void
Compilation::visit (AST::RangePatternBoundLiteral &bound)
{}
void
Compilation::visit (AST::RangePatternBoundPath &bound)
{}
void
Compilation::visit (AST::RangePatternBoundQualPath &bound)
{}
void
Compilation::visit (AST::RangePattern &pattern)
{}
void
Compilation::visit (AST::ReferencePattern &pattern)
{}
// void Compilation::visit(StructPatternField& field) {}
void
Compilation::visit (AST::StructPatternFieldTuplePat &field)
{}
void
Compilation::visit (AST::StructPatternFieldIdentPat &field)
{}
void
Compilation::visit (AST::StructPatternFieldIdent &field)
{}
void
Compilation::visit (AST::StructPattern &pattern)
{}
// void Compilation::visit(TupleStructItems& tuple_items) {}
void
Compilation::visit (AST::TupleStructItemsNoRange &tuple_items)
{}
void
Compilation::visit (AST::TupleStructItemsRange &tuple_items)
{}
void
Compilation::visit (AST::TupleStructPattern &pattern)
{}
// void Compilation::visit(TuplePatternItems& tuple_items) {}
void
Compilation::visit (AST::TuplePatternItemsMultiple &tuple_items)
{}
void
Compilation::visit (AST::TuplePatternItemsRanged &tuple_items)
{}
void
Compilation::visit (AST::TuplePattern &pattern)
{}
void
Compilation::visit (AST::GroupedPattern &pattern)
{}
void
Compilation::visit (AST::SlicePattern &pattern)
{}

// rust-stmt.h
void
Compilation::visit (AST::EmptyStmt &stmt)
{}
void

Compilation::visit (AST::LetStmt &stmt)
{
  if (!stmt.has_init_expr ())
    return;

  stmt.get_pattern ()->accept_vis (*this);
  for (auto &pattern : patternBuffer)
    {
      Bvariable *var = NULL;
      if (!scope.LookupVar (pattern.get_ident (), &var))
	{
	  rust_error_at (stmt.get_locus (), "failed to find var decl for %s",
			 pattern.get_ident ().c_str ());
	  return;
	}

      varBuffer.push_back (var);

      Bexpression *init = NULL;
      VISIT_POP (stmt.get_init_expr ()->get_locus_slow (), stmt.get_init_expr (), init,
		 exprs);
      if (init == NULL)
	{
	  rust_error_at (stmt.get_init_expr ()->get_locus_slow (),
			 "failed to compile init statement");
	  return;
	}

      auto s = backend->init_statement (scope.GetCurrentFndecl (), var, init);
      scope.AddStatement (s);

      varBuffer.pop_back ();
    }
  patternBuffer.clear ();
}

void
Compilation::visit (AST::ExprStmtWithoutBlock &stmt)
{
  stmt.get_expr ()->accept_vis (*this);
}

void
Compilation::visit (AST::ExprStmtWithBlock &stmt)
{
  Bblock *enclosingScope = NULL;
  Location start_location; /* = stmt.locus; FIXME */
  Location end_location;   // FIXME

  std::vector<Bvariable *> vars;
  auto code_block
    = backend->block (scope.GetCurrentFndecl (), scope.CurBlock (), vars,
		      start_location, end_location);

  scope.PushBlock (code_block);
  stmt.get_expr ()->accept_vis (*this);

  // get trailing if required
  for (auto &s : stmts)
    {
      scope.AddStatement (s);
    }
  stmts.clear ();

  scope.PopBlock ();

  auto body = backend->block_statement (code_block);
  scope.AddStatement (body);
}

// rust-type.h
void
Compilation::visit (AST::TraitBound &bound)
{}
void
Compilation::visit (AST::ImplTraitType &type)
{}
void
Compilation::visit (AST::TraitObjectType &type)
{}
void
Compilation::visit (AST::ParenthesisedType &type)
{}
void
Compilation::visit (AST::ImplTraitTypeOneBound &type)
{}
void
Compilation::visit (AST::TraitObjectTypeOneBound &type)
{}
void
Compilation::visit (AST::TupleType &type)
{}
void
Compilation::visit (AST::NeverType &type)
{}
void
Compilation::visit (AST::RawPointerType &type)
{}
void
Compilation::visit (AST::ReferenceType &type)
{}

void
Compilation::visit (AST::ArrayType &type)
{
  Btype *elementType;
  translatedType = nullptr;
  type.get_elem_type ()->accept_vis (*this);
  if (translatedType == nullptr)
    {
      rust_error_at (type.get_locus (),
		     "Failed to compile element type for array");
      return;
    }
  elementType = translatedType;

  Bexpression *length = nullptr;
  VISIT_POP (type.get_size_expr ()->get_locus_slow (), type.get_size_expr (),
	     length, exprs);
  if (length == nullptr)
    {
      rust_error_at (type.get_size_expr ()->get_locus_slow (),
		     "failed to size for array type");
      return;
    }

  translatedType = backend->array_type (elementType, length);
}

void
Compilation::visit (AST::SliceType &type)
{}
void
Compilation::visit (AST::InferredType &type)
{}
void
Compilation::visit (AST::BareFunctionType &type)
{}

} // namespace Compile
} // namespace Rust
