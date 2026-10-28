/* TJpgDec R0.03: Paperdesk's fixed, portable grayscale configuration. */
#ifndef PAPERDESK_TJPGDCNF_H
#define PAPERDESK_TJPGDCNF_H
#define JD_SZBUF 1024
#define JD_FORMAT 2
#define JD_USE_SCALE 1
#define JD_TBLCLIP 0
#define JD_FASTDECODE 1
#define JD_DEFAULT_HUFFMAN 0
/* Avoid accidental binding to an incompatible ROM decoder. */
#define jd_prepare paperdesk_jd_prepare
#define jd_decomp paperdesk_jd_decomp
#endif
