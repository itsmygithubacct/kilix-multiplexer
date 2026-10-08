#ifndef KMX_TEST_ENCODEC_DOUBLE_H
#define KMX_TEST_ENCODEC_DOUBLE_H
/* Native ABI double only. It never opens graphs, receipts or installed assets. */
#include <kilix_encodec.h>
#include <kilix_encodec_content.h>
#include <stdbool.h>
void kenc_double_arm(void);
bool kenc_double_entered(void);
void kenc_double_release(void);
#endif
