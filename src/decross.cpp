#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined (DECROSS_X86)
#include <emmintrin.h>
#endif

#include <VapourSynth4.h>
#include <VSHelper4.h>


#ifdef _WIN32
#define FORCE_INLINE __forceinline
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#endif


typedef struct DeCrossData {
    VSNode *clip;
    const VSVideoInfo *vi;

    int nYThreshold;
    int nNoiseThreshold;
    int nMargin;
    bool bDebug;
} DeCrossData;


static FORCE_INLINE bool Diff(const uint8_t* pDiff0, const uint8_t* pDiff1, const int nPos, int& nMiniDiff) {
#if defined (DECROSS_X86)
    __m128i mDiff0 = _mm_loadl_epi64((const __m128i *)&pDiff0[nPos]);
    __m128i mDiff1 = _mm_loadl_epi64((const __m128i *)pDiff1);

    __m128i mDiff = _mm_sad_epu8(mDiff0, mDiff1);

    int nDiff = _mm_cvtsi128_si32(mDiff);
    if (nDiff < nMiniDiff) {
        nMiniDiff = nDiff;
        return true;
    }
    return false;
#else
    int nDiff = 0;

    for (int i = 0; i < 8; i++)
        nDiff += std::abs(pDiff0[i + nPos] - pDiff1[i]);

    if (nDiff < nMiniDiff) {
        nMiniDiff = nDiff;
        return true;
    }
    return false;
#endif
}


static FORCE_INLINE void EdgeCheck(const uint8_t* pSrc, uint8_t* pEdgeBuffer, const int nRowSizeU, const int nYThreshold, const int nMargin) {
#if defined (DECROSS_X86)
    __m128i mYThreshold = _mm_set1_epi8(nYThreshold - 128);
    __m128i bytes_128 = _mm_set1_epi8(128);

    for (int nX = 4; nX < nRowSizeU - 4; nX += 4) {
        __m128i mLeft   = _mm_loadl_epi64((const __m128i *)&pSrc[nX * 2 - 1]);
        __m128i mCenter = _mm_loadl_epi64((const __m128i *)&pSrc[nX * 2]);
        __m128i mRight  = _mm_loadl_epi64((const __m128i *)&pSrc[nX * 2 + 1]);

        __m128i mLeft_128 = _mm_sub_epi8(mLeft, bytes_128);
        __m128i mCenter_128 = _mm_sub_epi8(mCenter, bytes_128);
        __m128i mRight_128 = _mm_sub_epi8(mRight, bytes_128);

        __m128i abs_diff_left_right = _mm_or_si128(_mm_subs_epu8(mLeft, mRight),
                                                   _mm_subs_epu8(mRight, mLeft));
        abs_diff_left_right = _mm_sub_epi8(abs_diff_left_right, bytes_128);

        __m128i mEdge = _mm_and_si128(_mm_cmpgt_epi8(abs_diff_left_right, mYThreshold),
                                      _mm_or_si128(_mm_and_si128(_mm_cmpgt_epi8(mCenter_128, mLeft_128),
                                                                 _mm_cmpgt_epi8(mRight_128, mCenter_128)),
                                                   _mm_and_si128(_mm_cmpgt_epi8(mLeft_128, mCenter_128),
                                                                 _mm_cmpgt_epi8(mCenter_128, mRight_128))));

        mEdge = _mm_packs_epi16(mEdge, mEdge);

        for (int i = -nMargin; i <= nMargin; i++) {
            *(int *)&pEdgeBuffer[nX + i] = _mm_cvtsi128_si32(_mm_or_si128(_mm_cvtsi32_si128(*(const int *)&pEdgeBuffer[nX + i]),
                                                                          mEdge));
        }
    }
#else
    // mirror the sse code above: it tests every luma column of the group and then folds
    // each adjacent pair into one chroma column, so a chroma column is an edge when either
    // of the two luma columns it covers is one
    for (int nX = 4; nX < nRowSizeU - 4; nX += 4) {
        for (int x = nX; x < nX + 4; x++) {
            bool edge = false;

            for (int k = 0; k < 2; k++) {
                int left = pSrc[x * 2 + k - 1];
                int center = pSrc[x * 2 + k];
                int right = pSrc[x * 2 + k + 1];

                if (std::abs(left - right) > nYThreshold &&
                    ((center > left && right > center) || (left > center && center > right)))
                    edge = true;
            }

            for (int i = -nMargin; i <= nMargin; i++)
                pEdgeBuffer[x + i] = pEdgeBuffer[x + i] || edge;
        }
    }
#endif
}


static FORCE_INLINE void AverageChroma(const uint8_t *pSrcU, const uint8_t *pSrcV, const uint8_t *pSrcUMini, const uint8_t *pSrcVMini, uint8_t *pDestU, uint8_t *pDestV, const uint8_t *pEdgeBuffer, int nX) {
#if defined (DECROSS_X86)
    __m128i mSrcU = _mm_cvtsi32_si128(*(const int *)&pSrcU[nX]);
    __m128i mSrcV = _mm_cvtsi32_si128(*(const int *)&pSrcV[nX]);

    __m128i mSrcUMini = _mm_cvtsi32_si128(*(const int *)&pSrcUMini[nX]);
    __m128i mSrcVMini = _mm_cvtsi32_si128(*(const int *)&pSrcVMini[nX]);

    __m128i mEdge = _mm_cvtsi32_si128(*(const int *)&pEdgeBuffer[nX]);

    __m128i mBlendColorU = _mm_avg_epu8(mSrcU, mSrcUMini);
    __m128i mBlendColorV = _mm_avg_epu8(mSrcV, mSrcVMini);

    __m128i mask = _mm_cmpeq_epi8(mEdge, _mm_setzero_si128());

    __m128i mDestU = _mm_or_si128(_mm_and_si128(mask, mSrcU),
                                  _mm_andnot_si128(mask, mBlendColorU));
    __m128i mDestV = _mm_or_si128(_mm_and_si128(mask, mSrcV),
                                  _mm_andnot_si128(mask, mBlendColorV));

    *(int *)&pDestU[nX] = _mm_cvtsi128_si32(mDestU);
    *(int *)&pDestV[nX] = _mm_cvtsi128_si32(mDestV);
#else
    for (int i = 0; i < 4; i++) {
        if (pEdgeBuffer[nX + i] == 0) {
            pDestU[nX + i] = pSrcU[nX + i];
            pDestV[nX + i] = pSrcV[nX + i];
        } else {
            pDestU[nX + i] = (pSrcU[nX + i] + pSrcUMini[nX + i] + 1) >> 1;
            pDestV[nX + i] = (pSrcV[nX + i] + pSrcVMini[nX + i] + 1) >> 1;
        }
    }
#endif
}


static const VSFrame *VS_CC deCrossGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    (void)frameData;

    const DeCrossData *d = (const DeCrossData *)instanceData;

    if (activationReason == arInitial) {
        if (n == 0 || n >= d->vi->numFrames - 1) {
            vsapi->requestFrameFilter(n, d->clip, frameCtx);
            return nullptr;
        }

        vsapi->requestFrameFilter(n - 1, d->clip, frameCtx);
        vsapi->requestFrameFilter(n, d->clip, frameCtx);
        vsapi->requestFrameFilter(n + 1, d->clip, frameCtx);
    } else if (activationReason == arAllFramesReady) {
        const VSFrame *src = vsapi->getFrameFilter(n, d->clip, frameCtx);

        if (n == 0 || n >= d->vi->numFrames - 1)
            return src;

        const VSFrame *srcP = vsapi->getFrameFilter(n - 1, d->clip, frameCtx);
        const VSFrame *srcF = vsapi->getFrameFilter(n + 1, d->clip, frameCtx);


        VSFrame *dst = vsapi->copyFrame(src, core);

        const int nHeightU = vsapi->getFrameHeight(src, 1);
        const int nRowSizeU = vsapi->getFrameWidth(src, 1);
        const ptrdiff_t nSrcPitch = vsapi->getStride(src, 0);
        const ptrdiff_t nSrcPitch2 = nSrcPitch * 2;
        const ptrdiff_t nSrcPitchU = vsapi->getStride(src, 1);
        const ptrdiff_t nDestPitchU = vsapi->getStride(dst, 1);

        const int subSamplingH = d->vi->format.subSamplingH;

        // luma rows belonging to one chroma row, and the luma row of a given chroma row
        const int nLumaPerChroma = 1 << subSamplingH;

        // the vertical taps reach two luma rows above and below the luma row of the chroma
        // row being filtered, so filtering can only start once that fits inside the plane.
        // this is row 1 for 420 but row 2 for 422, where a chroma row is a single luma row
        const int nFirstRow = (2 + nLumaPerChroma - 1) / nLumaPerChroma;

        const ptrdiff_t nLumaTop = nSrcPitch * nLumaPerChroma * nFirstRow;
        const ptrdiff_t nChromaTop = nSrcPitchU * nFirstRow;

        const uint8_t* pSrc = vsapi->getReadPtr(src, 0) + nLumaTop;
        const uint8_t* pSrcP = vsapi->getReadPtr(srcP, 0) + nLumaTop;
        const uint8_t* pSrcF = vsapi->getReadPtr(srcF, 0) + nLumaTop;

        const uint8_t* pSrcTT = pSrc - nSrcPitch2;
        const uint8_t* pSrcBB = pSrc + nSrcPitch2;
        const uint8_t* pSrcPTT = pSrcP - nSrcPitch2;
        const uint8_t* pSrcPBB = pSrcP + nSrcPitch2;
        const uint8_t* pSrcFTT = pSrcF - nSrcPitch2;
        const uint8_t* pSrcFBB = pSrcF + nSrcPitch2;

        const uint8_t* pSrcT = pSrc - nSrcPitch;
        const uint8_t* pSrcB = pSrc + nSrcPitch;
        const uint8_t* pSrcPT = pSrcP - nSrcPitch;
        const uint8_t* pSrcPB = pSrcP + nSrcPitch;
        const uint8_t* pSrcFT = pSrcF - nSrcPitch;
        const uint8_t* pSrcFB = pSrcF + nSrcPitch;

        const uint8_t* pSrcU = vsapi->getReadPtr(src, 1) + nChromaTop;
        const uint8_t* pSrcUP = vsapi->getReadPtr(srcP, 1) + nChromaTop;
        const uint8_t* pSrcUF = vsapi->getReadPtr(srcF, 1) + nChromaTop;
        const uint8_t* pSrcV = vsapi->getReadPtr(src, 2) + nChromaTop;
        const uint8_t* pSrcVP = vsapi->getReadPtr(srcP, 2) + nChromaTop;
        const uint8_t* pSrcVF = vsapi->getReadPtr(srcF, 2) + nChromaTop;

        const uint8_t* pSrcUTT = pSrcU - nSrcPitchU;
        const uint8_t* pSrcUBB = pSrcU + nSrcPitchU;
        const uint8_t* pSrcUPTT = pSrcUP - nSrcPitchU;
        const uint8_t* pSrcUPBB = pSrcUP + nSrcPitchU;
        const uint8_t* pSrcUFTT = pSrcUF - nSrcPitchU;
        const uint8_t* pSrcUFBB = pSrcUF + nSrcPitchU;
        const uint8_t* pSrcVTT = pSrcV - nSrcPitchU;
        const uint8_t* pSrcVBB = pSrcV + nSrcPitchU;
        const uint8_t* pSrcVPTT = pSrcVP - nSrcPitchU;
        const uint8_t* pSrcVPBB = pSrcVP + nSrcPitchU;
        const uint8_t* pSrcVFTT = pSrcVF - nSrcPitchU;
        const uint8_t* pSrcVFBB = pSrcVF + nSrcPitchU;

        const uint8_t* pSrcUMini;
        const uint8_t* pSrcVMini;

        uint8_t* pDestU = vsapi->getWritePtr(dst, 1) + nDestPitchU * nFirstRow;
        uint8_t* pDestV = vsapi->getWritePtr(dst, 2) + nDestPitchU * nFirstRow;

        // EdgeCheck updates the mask four bytes at a time and spreads it by nMargin, so it
        // reaches up to nMargin + 3 bytes past the last column, which the padding absorbs
        const int nEdgeBufferSize = nRowSizeU + 8;

        uint8_t* pEdgeBuffer = (uint8_t *)malloc(nEdgeBufferSize);

        if (!pEdgeBuffer) {
            vsapi->setFilterError("DeCross: out of memory.", frameCtx);
            vsapi->freeFrame(dst);
            vsapi->freeFrame(srcP);
            vsapi->freeFrame(src);
            vsapi->freeFrame(srcF);
            return NULL;
        }

        for (int nY = nHeightU - 1 - nFirstRow; nY > 2; nY--) {
            memset(pEdgeBuffer, 0, nEdgeBufferSize);

            EdgeCheck(pSrc, pEdgeBuffer, nRowSizeU, d->nYThreshold, d->nMargin);

            if (d->bDebug) {
                for (int nX = 4; nX < nRowSizeU - 4; nX++) {
                    if (pEdgeBuffer[nX] != 0) {
                        pDestU[nX] = 128;
                        pDestV[nX] = 255;
                    }
                }
            } else {
                int nX2 = 0;
                for (int nX = 4; nX < nRowSizeU - 4; nX += 4) {
                    nX2 += 4 * 2;
                    if (*(int*)&pEdgeBuffer[nX] != 0) {
                        int nMiniDiff = d->nNoiseThreshold;
                        pSrcUMini = pSrcU;
                        pSrcVMini = pSrcV;

                        if (nY % 2 == 1) {
                            if (Diff(pSrcPTT + nX2, pSrcT + nX2, -6, nMiniDiff)) { pSrcUMini = pSrcUPTT - 3; pSrcVMini = pSrcVPTT - 3; }
                            if (Diff(pSrcPTT + nX2, pSrcT + nX2, -2, nMiniDiff)) { pSrcUMini = pSrcUPTT - 1; pSrcVMini = pSrcVPTT - 1; }
                            if (Diff(pSrcPT  + nX2, pSrcT + nX2, -4, nMiniDiff)) { pSrcUMini = pSrcUP   - 2; pSrcVMini = pSrcVP   - 2; }
                            if (Diff(pSrcPBB + nX2, pSrcT + nX2, -6, nMiniDiff)) { pSrcUMini = pSrcUPBB - 3; pSrcVMini = pSrcVPBB - 3; }
                            if (Diff(pSrcPBB + nX2, pSrcT + nX2, -2, nMiniDiff)) { pSrcUMini = pSrcUPBB - 1; pSrcVMini = pSrcVPBB - 1; }

                            if (Diff(pSrcTT + nX2, pSrcT + nX2, -6, nMiniDiff)) { pSrcUMini = pSrcUTT - 3; pSrcVMini = pSrcVTT - 3; }
                            if (Diff(pSrcTT + nX2, pSrcT + nX2, -2, nMiniDiff)) { pSrcUMini = pSrcUTT - 1; pSrcVMini = pSrcVTT - 1; }
                            if (Diff(pSrcT  + nX2, pSrcT + nX2, -4, nMiniDiff)) { pSrcUMini = pSrcU   - 2; pSrcVMini = pSrcV   - 2; }
                            if (Diff(pSrcBB + nX2, pSrcT + nX2, -6, nMiniDiff)) { pSrcUMini = pSrcUBB - 3; pSrcVMini = pSrcVBB - 3; }
                            if (Diff(pSrcBB + nX2, pSrcT + nX2, -2, nMiniDiff)) { pSrcUMini = pSrcUBB - 1; pSrcVMini = pSrcVBB - 1; }

                            if (Diff(pSrcFTT + nX2, pSrcT + nX2, -6, nMiniDiff)) { pSrcUMini = pSrcUFTT - 3; pSrcVMini = pSrcVFTT - 3; }
                            if (Diff(pSrcFTT + nX2, pSrcT + nX2, -2, nMiniDiff)) { pSrcUMini = pSrcUFTT - 1; pSrcVMini = pSrcVFTT - 1; }
                            if (Diff(pSrcFT  + nX2, pSrcT + nX2, -4, nMiniDiff)) { pSrcUMini = pSrcUF   - 2; pSrcVMini = pSrcVF   - 2; }
                            if (Diff(pSrcFBB + nX2, pSrcT + nX2, -6, nMiniDiff)) { pSrcUMini = pSrcUFBB - 3; pSrcVMini = pSrcVFBB - 3; }
                            if (Diff(pSrcFBB + nX2, pSrcT + nX2, -2, nMiniDiff)) { pSrcUMini = pSrcUFBB - 1; pSrcVMini = pSrcVFBB - 1; }

                            if (Diff(pSrcPT + nX2, pSrcT + nX2, -0, nMiniDiff)) { pSrcUMini = pSrcUP - 0; pSrcVMini = pSrcVP - 0; }
                            if (Diff(pSrcFT + nX2, pSrcT + nX2, -0, nMiniDiff)) { pSrcUMini = pSrcUF - 0; pSrcVMini = pSrcVF - 0; }
                            if (Diff(pSrcPB + nX2, pSrcB + nX2, -0, nMiniDiff)) { pSrcUMini = pSrcUP - 0; pSrcVMini = pSrcVP - 0; }
                            if (Diff(pSrcFB + nX2, pSrcB + nX2, -0, nMiniDiff)) { pSrcUMini = pSrcUF - 0; pSrcVMini = pSrcVF - 0; }

                            if (Diff(pSrcPTT + nX2, pSrcT + nX2, +6, nMiniDiff)) { pSrcUMini = pSrcUPTT + 3; pSrcVMini = pSrcVPTT + 3; }
                            if (Diff(pSrcPTT + nX2, pSrcT + nX2, +2, nMiniDiff)) { pSrcUMini = pSrcUPTT + 1; pSrcVMini = pSrcVPTT + 1; }
                            if (Diff(pSrcPT  + nX2, pSrcT + nX2, +4, nMiniDiff)) { pSrcUMini = pSrcUP   + 2; pSrcVMini = pSrcVP   + 2; }
                            if (Diff(pSrcPBB + nX2, pSrcT + nX2, +6, nMiniDiff)) { pSrcUMini = pSrcUPBB + 3; pSrcVMini = pSrcVPBB + 3; }
                            if (Diff(pSrcPBB + nX2, pSrcT + nX2, +2, nMiniDiff)) { pSrcUMini = pSrcUPBB + 1; pSrcVMini = pSrcVPBB + 1; }

                            if (Diff(pSrcTT + nX2, pSrcT + nX2, +6, nMiniDiff)) { pSrcUMini = pSrcUTT + 3; pSrcVMini = pSrcVTT + 3; }
                            if (Diff(pSrcTT + nX2, pSrcT + nX2, +2, nMiniDiff)) { pSrcUMini = pSrcUTT + 1; pSrcVMini = pSrcVTT + 1; }
                            if (Diff(pSrcT  + nX2, pSrcT + nX2, +4, nMiniDiff)) { pSrcUMini = pSrcU   + 2; pSrcVMini = pSrcV   + 2; }
                            if (Diff(pSrcBB + nX2, pSrcT + nX2, +6, nMiniDiff)) { pSrcUMini = pSrcUBB + 3; pSrcVMini = pSrcVBB + 3; }
                            if (Diff(pSrcBB + nX2, pSrcT + nX2, +2, nMiniDiff)) { pSrcUMini = pSrcUBB + 1; pSrcVMini = pSrcVBB + 1; }

                            if (Diff(pSrcFTT + nX2, pSrcT + nX2, +6, nMiniDiff)) { pSrcUMini = pSrcUFTT + 3; pSrcVMini = pSrcVFTT + 3; }
                            if (Diff(pSrcFTT + nX2, pSrcT + nX2, +2, nMiniDiff)) { pSrcUMini = pSrcUFTT + 1; pSrcVMini = pSrcVFTT + 1; }
                            if (Diff(pSrcFT  + nX2, pSrcT + nX2, +4, nMiniDiff)) { pSrcUMini = pSrcUF   + 2; pSrcVMini = pSrcVF   + 2; }
                            if (Diff(pSrcFBB + nX2, pSrcT + nX2, +6, nMiniDiff)) { pSrcUMini = pSrcUFBB + 3; pSrcVMini = pSrcVFBB + 3; }
                            if (Diff(pSrcFBB + nX2, pSrcT + nX2, +2, nMiniDiff)) { pSrcUMini = pSrcUFBB + 1; pSrcVMini = pSrcVFBB + 1; }
                        } else {
                            if (Diff(pSrcPT + nX2, pSrc + nX2, -6, nMiniDiff)) { pSrcUMini = pSrcUPTT - 3; pSrcVMini = pSrcVPTT - 3; }
                            if (Diff(pSrcPT + nX2, pSrc + nX2, -2, nMiniDiff)) { pSrcUMini = pSrcUPTT - 1; pSrcVMini = pSrcVPTT - 1; }
                            if (Diff(pSrcP  + nX2, pSrc + nX2, -4, nMiniDiff)) { pSrcUMini = pSrcUP   - 2; pSrcVMini = pSrcVP   - 2; }
                            if (Diff(pSrcPB + nX2, pSrc + nX2, -6, nMiniDiff)) { pSrcUMini = pSrcUPBB - 3; pSrcVMini = pSrcVPBB - 3; }
                            if (Diff(pSrcPB + nX2, pSrc + nX2, -2, nMiniDiff)) { pSrcUMini = pSrcUPBB - 1; pSrcVMini = pSrcVPBB - 1; }

                            if (Diff(pSrcT + nX2, pSrc + nX2, -6, nMiniDiff)) { pSrcUMini = pSrcUTT - 3; pSrcVMini = pSrcVTT - 3; }
                            if (Diff(pSrcT + nX2, pSrc + nX2, -2, nMiniDiff)) { pSrcUMini = pSrcUTT - 1; pSrcVMini = pSrcVTT - 1; }
                            if (Diff(pSrc  + nX2, pSrc + nX2, -4, nMiniDiff)) { pSrcUMini = pSrcU   - 2; pSrcVMini = pSrcV   - 2; }
                            if (Diff(pSrcB + nX2, pSrc + nX2, -6, nMiniDiff)) { pSrcUMini = pSrcUBB - 3; pSrcVMini = pSrcVBB - 3; }
                            if (Diff(pSrcB + nX2, pSrc + nX2, -2, nMiniDiff)) { pSrcUMini = pSrcUBB - 1; pSrcVMini = pSrcVBB - 1; }

                            if (Diff(pSrcFT + nX2, pSrc + nX2, -6, nMiniDiff)) { pSrcUMini = pSrcUFTT - 3; pSrcVMini = pSrcVFTT - 3; }
                            if (Diff(pSrcFT + nX2, pSrc + nX2, -2, nMiniDiff)) { pSrcUMini = pSrcUFTT - 1; pSrcVMini = pSrcVFTT - 1; }
                            if (Diff(pSrcF  + nX2, pSrc + nX2, -4, nMiniDiff)) { pSrcUMini = pSrcUF   - 2; pSrcVMini = pSrcVF   - 2; }
                            if (Diff(pSrcFB + nX2, pSrc + nX2, -6, nMiniDiff)) { pSrcUMini = pSrcUFBB - 3; pSrcVMini = pSrcVFBB - 3; }
                            if (Diff(pSrcFB + nX2, pSrc + nX2, -2, nMiniDiff)) { pSrcUMini = pSrcUFBB - 1; pSrcVMini = pSrcVFBB - 1; }

                            if (Diff(pSrcP + nX2, pSrc + nX2, -0, nMiniDiff)) { pSrcUMini = pSrcUP - 0; pSrcVMini = pSrcVP - 0; }
                            if (Diff(pSrcF + nX2, pSrc + nX2, -0, nMiniDiff)) { pSrcUMini = pSrcUF - 0; pSrcVMini = pSrcVF - 0; }

                            if (Diff(pSrcPT + nX2, pSrc + nX2, +6, nMiniDiff)) { pSrcUMini = pSrcUPTT + 3; pSrcVMini = pSrcVPTT + 3; }
                            if (Diff(pSrcPT + nX2, pSrc + nX2, +2, nMiniDiff)) { pSrcUMini = pSrcUPTT + 1; pSrcVMini = pSrcVPTT + 1; }
                            if (Diff(pSrcP  + nX2, pSrc + nX2, +4, nMiniDiff)) { pSrcUMini = pSrcUP   + 2; pSrcVMini = pSrcVP   + 2; }
                            if (Diff(pSrcPB + nX2, pSrc + nX2, +6, nMiniDiff)) { pSrcUMini = pSrcUPBB + 3; pSrcVMini = pSrcVPBB + 3; }
                            if (Diff(pSrcPB + nX2, pSrc + nX2, +2, nMiniDiff)) { pSrcUMini = pSrcUPBB + 1; pSrcVMini = pSrcVPBB + 1; }

                            if (Diff(pSrcT + nX2, pSrc + nX2, +6, nMiniDiff)) { pSrcUMini = pSrcUTT + 3; pSrcVMini = pSrcVTT + 3; }
                            if (Diff(pSrcT + nX2, pSrc + nX2, +2, nMiniDiff)) { pSrcUMini = pSrcUTT + 1; pSrcVMini = pSrcVTT + 1; }
                            if (Diff(pSrc  + nX2, pSrc + nX2, +4, nMiniDiff)) { pSrcUMini = pSrcU   + 2; pSrcVMini = pSrcV   + 2; }
                            if (Diff(pSrcB + nX2, pSrc + nX2, +6, nMiniDiff)) { pSrcUMini = pSrcUBB + 3; pSrcVMini = pSrcVBB + 3; }
                            if (Diff(pSrcB + nX2, pSrc + nX2, +2, nMiniDiff)) { pSrcUMini = pSrcUBB + 1; pSrcVMini = pSrcVBB + 1; }

                            if (Diff(pSrcFT + nX2, pSrc + nX2, +6, nMiniDiff)) { pSrcUMini = pSrcUFTT + 3; pSrcVMini = pSrcVFTT + 3; }
                            if (Diff(pSrcFT + nX2, pSrc + nX2, +2, nMiniDiff)) { pSrcUMini = pSrcUFTT + 1; pSrcVMini = pSrcVFTT + 1; }
                            if (Diff(pSrcF  + nX2, pSrc + nX2, +4, nMiniDiff)) { pSrcUMini = pSrcUF   + 2; pSrcVMini = pSrcVF   + 2; }
                            if (Diff(pSrcFB + nX2, pSrc + nX2, +6, nMiniDiff)) { pSrcUMini = pSrcUFBB + 3; pSrcVMini = pSrcVFBB + 3; }
                            if (Diff(pSrcFB + nX2, pSrc + nX2, +2, nMiniDiff)) { pSrcUMini = pSrcUFBB + 1; pSrcVMini = pSrcVFBB + 1; }
                        }

                        AverageChroma(pSrcU, pSrcV, pSrcUMini, pSrcVMini, pDestU, pDestV, pEdgeBuffer, nX);
                    }
                }
            }

            pSrc += nSrcPitch << subSamplingH;
            pSrcP += nSrcPitch << subSamplingH;
            pSrcF += nSrcPitch << subSamplingH;

            pSrcTT += nSrcPitch << subSamplingH;
            pSrcBB += nSrcPitch << subSamplingH;
            pSrcPTT += nSrcPitch << subSamplingH;
            pSrcPBB += nSrcPitch << subSamplingH;
            pSrcFTT += nSrcPitch << subSamplingH;
            pSrcFBB += nSrcPitch << subSamplingH;

            pSrcT += nSrcPitch << subSamplingH;
            pSrcB += nSrcPitch << subSamplingH;
            pSrcPT += nSrcPitch << subSamplingH;
            pSrcPB += nSrcPitch << subSamplingH;
            pSrcFT += nSrcPitch << subSamplingH;
            pSrcFB += nSrcPitch << subSamplingH;

            pSrcU += nSrcPitchU;
            pSrcUP += nSrcPitchU;
            pSrcUF += nSrcPitchU;
            pSrcV += nSrcPitchU;
            pSrcVP += nSrcPitchU;
            pSrcVF += nSrcPitchU;

            pSrcUTT += nSrcPitchU;
            pSrcUBB += nSrcPitchU;
            pSrcUPTT += nSrcPitchU;
            pSrcUPBB += nSrcPitchU;
            pSrcUFTT += nSrcPitchU;
            pSrcUFBB += nSrcPitchU;
            pSrcVTT += nSrcPitchU;
            pSrcVBB += nSrcPitchU;
            pSrcVPTT += nSrcPitchU;
            pSrcVPBB += nSrcPitchU;
            pSrcVFTT += nSrcPitchU;
            pSrcVFBB += nSrcPitchU;

            pDestU += nDestPitchU;
            pDestV += nDestPitchU;
        }

        free(pEdgeBuffer);

        vsapi->freeFrame(srcP);
        vsapi->freeFrame(src);
        vsapi->freeFrame(srcF);

        return dst;
    }

    return NULL;
}


static void VS_CC deCrossFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    (void)core;

    DeCrossData *d = (DeCrossData *)instanceData;

    vsapi->freeNode(d->clip);
    free(d);
}


static void VS_CC deCrossCreate(const VSMap *in, VSMap *out, void *userData, VSCore *core, const VSAPI *vsapi) {
    (void)userData;

    DeCrossData d;
    memset(&d, 0, sizeof(d));

    int err;

    d.nYThreshold = vsapi->mapGetIntSaturated(in, "thresholdy", 0, &err);
    if (err)
        d.nYThreshold = 30;

    d.nNoiseThreshold = vsapi->mapGetIntSaturated(in, "noise", 0, &err);
    if (err)
        d.nNoiseThreshold = 60;

    d.nMargin = vsapi->mapGetIntSaturated(in, "margin", 0, &err);
    if (err)
        d.nMargin = 1;

    d.bDebug = !!vsapi->mapGetInt(in, "debug", 0, &err);


    if (d.nYThreshold < 0 || d.nYThreshold > 255) {
        vsapi->mapSetError(out, "DeCross: thresholdy must be between 0 and 255 (inclusive).");
        return;
    }

    if (d.nNoiseThreshold < 0 || d.nNoiseThreshold > 255) {
        vsapi->mapSetError(out, "DeCross: noise must be between 0 and 255 (inclusive).");
        return;
    }

    if (d.nMargin < 0 || d.nMargin > 4) {
        vsapi->mapSetError(out, "DeCross: margin must be between 0 and 4 (inclusive).");
        return;
    }


    d.clip = vsapi->mapGetNode(in, "clip", 0, NULL);
    d.vi = vsapi->getVideoInfo(d.clip);

    uint32_t formatId = 0;

    if (vsh::isConstantVideoFormat(d.vi))
        formatId = vsapi->queryVideoFormatID(d.vi->format.colorFamily, d.vi->format.sampleType, d.vi->format.bitsPerSample,
                                             d.vi->format.subSamplingW, d.vi->format.subSamplingH, core);

    if (formatId != pfYUV420P8 && formatId != pfYUV422P8) {
        vsapi->mapSetError(out, "DeCross: only YUV420P8 and YUV422P8 with constant format and dimensions supported.");
        vsapi->freeNode(d.clip);
        return;
    }


    DeCrossData *data = (DeCrossData *)malloc(sizeof(d));

    if (!data) {
        vsapi->mapSetError(out, "DeCross: out of memory.");
        vsapi->freeNode(d.clip);
        return;
    }

    *data = d;

    VSFilterDependency deps[] = { {data->clip, rpGeneral} };

    vsapi->createVideoFilter(out, "DeCross", data->vi, deCrossGetFrame, deCrossFree, fmParallel, deps, 1, data, core);
}


VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin("com.nodame.decross", "decross", "Spatio-temporal derainbow filter", VS_MAKE_VERSION(2, 0), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("DeCross",
                             "clip:vnode;"
                             "thresholdy:int:opt;"
                             "noise:int:opt;"
                             "margin:int:opt;"
                             "debug:int:opt;",
                             "clip:vnode;"
                             , deCrossCreate, 0, plugin);
}
