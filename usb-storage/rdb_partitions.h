#ifndef RDB_PARTITIONS_H
#define RDB_PARTITIONS_H

int parse_rdb(struct Library* ExpansionBase, struct ConfigDev* cd);
void debugstr(void* regs, char* str);
void debughex(void* regs, uint32_t val);

#endif
