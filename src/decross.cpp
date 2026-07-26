#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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


// the sum of eight differences stays well inside an int even at 16 bit (8 * 65535)
template <typename T>
static FORCE_INLINE bool Diff(const T* pDiff0, const T* pDiff1, const int nPos, int& nMiniDiff) {
    int nDiff = 0;

    for (int i = 0; i < 8; i++)
        nDiff += std::abs(pDiff0[i + nPos] - pDiff1[i]);

    if (nDiff < nMiniDiff) {
        nMiniDiff = nDiff;
        return true;
    }
    return false;
}


// the mask buffer is a separate allocation from the frame planes, so the stores into it
// can never disturb the luma loads. that matters most at 8 bit, where both are unsigned
// char and the compiler otherwise has to assume every store aliases every load
template <typename T>
static FORCE_INLINE void EdgeCheck(const T* __restrict pSrc, uint8_t* __restrict pEdgeBuffer, const int nRowSizeU, const int nYThreshold, const int nMargin) {
    // a chroma column covers two luma columns and is an edge when either of them is one
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
}


// only the destinations are marked: they are distinct planes of a frame that copyFrame has
// already un-shared, so they alias neither each other nor any source. the sources are left
// alone on purpose because pSrcUMini is allowed to be pSrcU itself
template <typename T>
static FORCE_INLINE void AverageChroma(const T *pSrcU, const T *pSrcV, const T *pSrcUMini, const T *pSrcVMini, T * __restrict pDestU, T * __restrict pDestV, const uint8_t *pEdgeBuffer, int nX) {
    for (int i = 0; i < 4; i++) {
        if (pEdgeBuffer[nX + i] == 0) {
            pDestU[nX + i] = pSrcU[nX + i];
            pDestV[nX + i] = pSrcV[nX + i];
        } else {
            pDestU[nX + i] = (T)((pSrcU[nX + i] + pSrcUMini[nX + i] + 1) >> 1);
            pDestV[nX + i] = (T)((pSrcV[nX + i] + pSrcVMini[nX + i] + 1) >> 1);
        }
    }
}


template <typename T>
static void DeCrossFrame(const DeCrossData *d, const VSFrame *src, const VSFrame *srcP, const VSFrame *srcF, VSFrame *dst, uint8_t *pEdgeBuffer, const int nEdgeBufferSize, const VSAPI *vsapi) {
    {
        // pitches are counted in samples so the same pointer maths works at every depth
        const int nHeightU = vsapi->getFrameHeight(src, 1);
        const int nRowSizeU = vsapi->getFrameWidth(src, 1);
        const ptrdiff_t nSampleSize = (ptrdiff_t)sizeof(T);
        const ptrdiff_t nSrcPitch = vsapi->getStride(src, 0) / nSampleSize;
        const ptrdiff_t nSrcPitch2 = nSrcPitch * 2;
        const ptrdiff_t nSrcPitchU = vsapi->getStride(src, 1) / nSampleSize;
        const ptrdiff_t nDestPitchU = vsapi->getStride(dst, 1) / nSampleSize;

        const int subSamplingH = d->vi->format.subSamplingH;

        // luma rows belonging to one chroma row, and the luma row of a given chroma row
        const int nLumaPerChroma = 1 << subSamplingH;

        // the vertical taps reach two luma rows above and below the luma row of the chroma
        // row being filtered, so filtering can only start once that fits inside the plane.
        // this is row 1 for 420 but row 2 for 422, where a chroma row is a single luma row
        const int nFirstRow = (2 + nLumaPerChroma - 1) / nLumaPerChroma;

        const ptrdiff_t nLumaTop = nSrcPitch * nLumaPerChroma * nFirstRow;
        const ptrdiff_t nChromaTop = nSrcPitchU * nFirstRow;

        const T* pSrc = (const T *)vsapi->getReadPtr(src, 0) + nLumaTop;
        const T* pSrcP = (const T *)vsapi->getReadPtr(srcP, 0) + nLumaTop;
        const T* pSrcF = (const T *)vsapi->getReadPtr(srcF, 0) + nLumaTop;

        const T* pSrcTT = pSrc - nSrcPitch2;
        const T* pSrcBB = pSrc + nSrcPitch2;
        const T* pSrcPTT = pSrcP - nSrcPitch2;
        const T* pSrcPBB = pSrcP + nSrcPitch2;
        const T* pSrcFTT = pSrcF - nSrcPitch2;
        const T* pSrcFBB = pSrcF + nSrcPitch2;

        const T* pSrcT = pSrc - nSrcPitch;
        const T* pSrcB = pSrc + nSrcPitch;
        const T* pSrcPT = pSrcP - nSrcPitch;
        const T* pSrcPB = pSrcP + nSrcPitch;
        const T* pSrcFT = pSrcF - nSrcPitch;
        const T* pSrcFB = pSrcF + nSrcPitch;

        const T* pSrcU = (const T *)vsapi->getReadPtr(src, 1) + nChromaTop;
        const T* pSrcUP = (const T *)vsapi->getReadPtr(srcP, 1) + nChromaTop;
        const T* pSrcUF = (const T *)vsapi->getReadPtr(srcF, 1) + nChromaTop;
        const T* pSrcV = (const T *)vsapi->getReadPtr(src, 2) + nChromaTop;
        const T* pSrcVP = (const T *)vsapi->getReadPtr(srcP, 2) + nChromaTop;
        const T* pSrcVF = (const T *)vsapi->getReadPtr(srcF, 2) + nChromaTop;

        const T* pSrcUTT = pSrcU - nSrcPitchU;
        const T* pSrcUBB = pSrcU + nSrcPitchU;
        const T* pSrcUPTT = pSrcUP - nSrcPitchU;
        const T* pSrcUPBB = pSrcUP + nSrcPitchU;
        const T* pSrcUFTT = pSrcUF - nSrcPitchU;
        const T* pSrcUFBB = pSrcUF + nSrcPitchU;
        const T* pSrcVTT = pSrcV - nSrcPitchU;
        const T* pSrcVBB = pSrcV + nSrcPitchU;
        const T* pSrcVPTT = pSrcVP - nSrcPitchU;
        const T* pSrcVPBB = pSrcVP + nSrcPitchU;
        const T* pSrcVFTT = pSrcVF - nSrcPitchU;
        const T* pSrcVFBB = pSrcVF + nSrcPitchU;

        const T* pSrcUMini;
        const T* pSrcVMini;

        // nothing else in this function touches the destination planes
        T* __restrict pDestU = (T *)vsapi->getWritePtr(dst, 1) + nDestPitchU * nFirstRow;
        T* __restrict pDestV = (T *)vsapi->getWritePtr(dst, 2) + nDestPitchU * nFirstRow;

        // debug marks the filtered columns with neutral U and maximum V for the depth
        const T nDebugU = (T)(1 << (d->vi->format.bitsPerSample - 1));
        const T nDebugV = (T)((1 << d->vi->format.bitsPerSample) - 1);

        for (int nY = nHeightU - 1 - nFirstRow; nY > 2; nY--) {
            memset(pEdgeBuffer, 0, nEdgeBufferSize);

            EdgeCheck(pSrc, pEdgeBuffer, nRowSizeU, d->nYThreshold, d->nMargin);

            if (d->bDebug) {
                for (int nX = 4; nX < nRowSizeU - 4; nX++) {
                    if (pEdgeBuffer[nX] != 0) {
                        pDestU[nX] = nDebugU;
                        pDestV[nX] = nDebugV;
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
    }
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

        // EdgeCheck walks the columns in groups of four and spreads each group by nMargin,
        // so it reaches up to nMargin + 3 past the last column, which the padding absorbs
        const int nEdgeBufferSize = vsapi->getFrameWidth(src, 1) + 8;

        uint8_t* pEdgeBuffer = (uint8_t *)malloc(nEdgeBufferSize);

        if (!pEdgeBuffer) {
            vsapi->setFilterError("DeCross: out of memory.", frameCtx);
            vsapi->freeFrame(dst);
            vsapi->freeFrame(srcP);
            vsapi->freeFrame(src);
            vsapi->freeFrame(srcF);
            return NULL;
        }

        if (d->vi->format.bytesPerSample == 1)
            DeCrossFrame<uint8_t>(d, src, srcP, srcF, dst, pEdgeBuffer, nEdgeBufferSize, vsapi);
        else
            DeCrossFrame<uint16_t>(d, src, srcP, srcF, dst, pEdgeBuffer, nEdgeBufferSize, vsapi);

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


    if (d.nMargin < 0 || d.nMargin > 4) {
        vsapi->mapSetError(out, "DeCross: margin must be between 0 and 4 (inclusive).");
        return;
    }


    d.clip = vsapi->mapGetNode(in, "clip", 0, NULL);
    d.vi = vsapi->getVideoInfo(d.clip);

    if (!vsh::isConstantVideoFormat(d.vi) ||
        d.vi->format.colorFamily != cfYUV ||
        d.vi->format.sampleType != stInteger ||
        d.vi->format.bitsPerSample < 8 ||
        d.vi->format.bitsPerSample > 16 ||
        d.vi->format.subSamplingW != 1 ||
        d.vi->format.subSamplingH > 1) {
        vsapi->mapSetError(out, "DeCross: only 8..16 bit integer YUV 4:2:0 and 4:2:2 with constant format and dimensions supported.");
        vsapi->freeNode(d.clip);
        return;
    }

    // the thresholds are compared against raw sample values, so their range follows the depth
    const int nMaxValue = (1 << d.vi->format.bitsPerSample) - 1;
    char szError[128];

    if (d.nYThreshold < 0 || d.nYThreshold > nMaxValue) {
        snprintf(szError, sizeof(szError), "DeCross: thresholdy must be between 0 and %d (inclusive) for this clip.", nMaxValue);
        vsapi->mapSetError(out, szError);
        vsapi->freeNode(d.clip);
        return;
    }

    if (d.nNoiseThreshold < 0 || d.nNoiseThreshold > nMaxValue) {
        snprintf(szError, sizeof(szError), "DeCross: noise must be between 0 and %d (inclusive) for this clip.", nMaxValue);
        vsapi->mapSetError(out, szError);
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
    vspapi->configPlugin("com.nodame.decross", "decross", "Spatio-temporal derainbow filter", VS_MAKE_VERSION(3, 0), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("DeCross",
                             "clip:vnode;"
                             "thresholdy:int:opt;"
                             "noise:int:opt;"
                             "margin:int:opt;"
                             "debug:int:opt;",
                             "clip:vnode;"
                             , deCrossCreate, 0, plugin);
}
