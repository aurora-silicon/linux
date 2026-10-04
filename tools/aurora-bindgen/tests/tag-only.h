struct payload { unsigned long words[20]; };
struct harmless_tag { unsigned before; struct payload; unsigned after; };
struct ordinary_forward { struct forward_tag; void *ptr; unsigned after; };
struct normal_named { unsigned before; struct payload named; unsigned after; };
