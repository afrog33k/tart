/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#ifndef TART_TYPE_TYPERELATION_H
#define TART_TYPE_TYPERELATION_H

#ifndef TART_TYPE_TYPE_H
#include "tart/Type/Type.h"
#endif

namespace tart {

/// -------------------------------------------------------------------
/// Various binary relations on types. All of these operators do
/// maximal dereferencing on their arguments, meaning that if the
/// argument is an alias, an assignment, or an ambiguous type, the
/// operator will attempt to "drill down" into the definition as
/// deeply as possible.
namespace TypeRelation {

enum Options {
  VERBOSE = (1<<0),                         // Report why we didn't match
  DISTINGUISH_VARIADIC_PARAMS = (1<<1),     // Functions different based on variadic flag
  DISTINGUISH_SELF_PARAM = (1<<2),          // Functions different based on self param
  MATCH_ALL_AMBIG = (1<<3),                 // Ambiguous types must match *all* possibles
  EXACT_ASSIGNMENT = (1<<4),                // Type assignment matches must be strict

  DEFAULT = MATCH_ALL_AMBIG,
};

/** Returns true if the type on the left is equal to the type on
    the right. */
bool isEqual(const Type * lhs, const Type * rhs);

/** Returns true if the type on the left is either the same as,
    or is a subtype of, the type on the right. Note that in the case of
    ambiguous types, all possibilities must pass the subclass test
    in order for the whole to be considered a subtype. */
bool isSubtype(const Type * lhs, const Type * rhs);

} // namespace TypeRelation
} // namespace tart

#endif // TART_TYPE_TYPERELATION_H
