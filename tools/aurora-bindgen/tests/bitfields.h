struct bitfields {
    unsigned int enabled : 1;
    unsigned int count : 7;
    signed int delta : 8;
    unsigned int reserved : 16;
};
struct bitfield_container {
    unsigned long before;
    struct bitfields value;
    unsigned long after;
};
