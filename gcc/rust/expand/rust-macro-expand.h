#ifndef RUST_MACRO_EXPAND_H
#define RUST_MACRO_EXPAND_H

#include "rust-ast.h"

// Provides objects and method prototypes for macro expansion

namespace Rust {
// forward decls for AST
namespace AST {
class MacroInvocation;
}

// Object used to store configuration data for macro expansion.
struct ExpansionCfg
{
  // features?
  unsigned int recursion_limit = 50; // TODO: determine default recursion limit
				// trace macros?
				// should test?
				// more default stuff?
};

// Object used to store shared data (between functions) for macro expansion.
struct MacroExpander
{
  ExpansionCfg cfg;
  unsigned int expansion_depth = 0;

  MacroExpander (AST::Crate &crate, ExpansionCfg cfg, Session &session)
    : cfg (cfg), crate (crate), session (session)
  {}

  ~MacroExpander () = default;

  // Expands all macros in the crate passed in.
  void expand_crate ();

  /* Expands a macro invocation (not macro invocation semi) - possibly make both
   * have similar duck-typed interface and use templates?*/
  // should this be public or private?
  void expand_invoc (std::unique_ptr<AST::MacroInvocation> &invoc);

  void expand_cfg_attrs (std::vector<AST::Attribute> &attrs);
  bool fails_cfg (std::vector<AST::Attribute> &attr);

  /* TODO: make it extend ASTVisitor so that individual items can be accessed
   * properly? */

private:
  AST::Crate &crate;
  Session &session;
};
} // namespace Rust

#endif
