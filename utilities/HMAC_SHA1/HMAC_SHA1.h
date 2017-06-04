/*
	100% free public domain implementation of the HMAC-SHA1 algorithm
	by Chien-Chung, Chung (Jim Chung) <jimchung1221@gmail.com>
*/


#ifndef __HMAC_SHA1_H__
#define __HMAC_SHA1_H__

#include "SHA1.h"

typedef unsigned char BYTE ;

class CHMAC_SHA1 : public CSHA1
{
    private:
		BYTE m_ipad[64];
        BYTE m_opad[64];
		enum {
			SHA1_DIGEST_LENGTH	= 20,
			SHA1_BLOCK_SIZE		= 64,
			HMAC_BUF_LEN		= 4096
		} ;

		char szReport[HMAC_BUF_LEN];
		char SHA1_Key[HMAC_BUF_LEN];
		char AppendBuf1[HMAC_BUF_LEN];
		char AppendBuf2[HMAC_BUF_LEN];


	public:
		

		CHMAC_SHA1(){}

        ~CHMAC_SHA1(){}

        void HMAC_SHA1(BYTE *text, int text_len, const char *key, int key_len, BYTE *digest);
};


#endif /* __HMAC_SHA1_H__ */
