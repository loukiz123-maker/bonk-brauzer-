/*---
description: a syntax error is a parse-phase failure
negative:
  phase: parse
  type: SyntaxError
---*/
var broken = ;
