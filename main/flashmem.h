#ifndef __FLASHMEM__
#define __FLASHMEM__


extern void flash_open(char *name);
extern void flash_erase_all(void);
extern uint16_t flash_read(char *name, uint16_t def);
extern void flash_write(char *name, uint16_t value);
extern char *flash_read_str(char *name, char *def, int len);
extern void flash_write_str(char *name, char *value);
extern void flash_write_float(char *name, float value);
extern float flash_read_float(char *name, float def);
extern void flash_commitchanges(void);

#endif
