/*---
description: basic arithmetic and assertion shim
---*/
var sum = 0;
for (var i = 1; i <= 10; i++) sum += i;
assert.sameValue(sum, 55, "sum");
