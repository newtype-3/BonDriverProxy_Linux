////////////////////////////////////////////////////////////////////////////////
// B25Decoder.h
////////////////////////////////////////////////////////////////////////////////
#ifndef __B25DECODER_H__
#define __B25DECODER_H__
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <aribb25/arib_std_b25.h>
#include <aribb25/arib_std_b25_error_code.h>

#define RETRY_INTERVAL	10	// 10sec interval

class B25Decoder
{
public:
	B25Decoder();
	~B25Decoder();
	int init();
	void setemm(bool flag);
	void decode(unsigned char *pSrc, unsigned int dwSrcSize, unsigned char **ppDst, unsigned int *pdwDstSize);

	// libaribb25 wrapper
	int reset();
	int flush();
	int put(unsigned char *pSrc, unsigned int dwSrcSize);
	int get(unsigned char **ppDst, unsigned int *pdwDstSize);

	// initialize parameter
	static int strip;
	static int emm_proc;
	static int multi2_round;

private:
	pthread_mutex_t _mtx;
	B_CAS_CARD *_bcas;
	ARIB_STD_B25 *_b25;
	unsigned char *_data;
	struct timespec _errtime;
};

#endif	// __B25DECODER_H__
