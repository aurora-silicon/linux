struct payload { unsigned long words[20]; };
union tag_union { unsigned long word; unsigned char bytes[24]; };
struct outer { unsigned before; union { struct payload; void *ptr; }; unsigned after; };
struct wrapper { unsigned prefix; struct payload; unsigned suffix; };
struct union_member { char prefix; union tag_union; unsigned suffix; };
struct normal_named { char prefix; struct payload named; unsigned suffix; };
struct anonymous_definition { char prefix; struct { unsigned long inner; }; unsigned suffix; };
struct forward_only { struct never_defined *ptr; unsigned suffix; };
struct trailing_member { unsigned prefix; struct payload; };
