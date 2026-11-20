typedef unsigned long ERR_STATE;
#define ERR_LIB_NONE 0
#define ERR_LIB_SSL 20
#define ERR_LIB_SYS 2
#define ERR_LIB_X509 11
#define ERR_LIB_PEM 9
#define ERR_LIB_X 12
#define ERR_GET_LIB(e) (((e)>>23)&0xff)
#define ERR_GET_REASON(e) ((e)&0x7fffff)
#define ERR_PACK(l,r) ((int)(((unsigned int)(l)<<23)|(unsigned int)(r)))
unsigned long ERR_get_error(void);
unsigned long ERR_peek_last_error(void);
void ERR_clear_error(void);
const char *ERR_reason_error_string(unsigned long e);
