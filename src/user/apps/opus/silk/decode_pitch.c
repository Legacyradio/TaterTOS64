/***********************************************************************
Copyright (c) 2006-2011, Skype Limited. All rights reserved.
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
- Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
- Neither the name of Internet Society, IETF or IETF Trust, nor the
names of specific contributors, may be used to endorse or promote
products derived from this software without specific prior written
permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
***********************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "main.h"
#include "pitch_est_defines.h"

/*
 * Decode pitch lag and contour index into pitch lags for each subframe.
 */
void silk_decode_pitch(
    opus_int16          lagIndex,       /* I    Lag index                           */
    opus_int8           contourIndex,   /* I    Contour index                       */
    opus_int            pitchL[],       /* O    Pitch lag values for each subframe  */
    const opus_int      Fs_kHz,         /* I    Sampling frequency (kHz)            */
    const opus_int      nb_subfr        /* I    Number of subframes                 */
)
{
    opus_int lag, k, min_lag, max_lag;
    const opus_int8 *Lag_CB_ptr;

    if( Fs_kHz == 8 ) {
        if( nb_subfr == 4 ) {
            Lag_CB_ptr = &silk_CB_lags_stage2[ 0 ][ 0 ];
        } else {
            Lag_CB_ptr = &silk_CB_lags_stage2_10_ms[ 0 ][ 0 ];
        }
    } else {
        if( nb_subfr == 4 ) {
            Lag_CB_ptr = &silk_CB_lags_stage3[ 0 ][ 0 ];
        } else {
            Lag_CB_ptr = &silk_CB_lags_stage3_10_ms[ 0 ][ 0 ];
        }
    }

    min_lag = silk_SMULBB( 2, Fs_kHz );
    max_lag = silk_SMULBB( 18, Fs_kHz );
    lag = min_lag + lagIndex;

    for( k = 0; k < nb_subfr; k++ ) {
        pitchL[ k ] = lag + Lag_CB_ptr[ (opus_int)contourIndex * nb_subfr + k ];
        if( pitchL[ k ] < min_lag ) {
            pitchL[ k ] = min_lag;
        }
        if( pitchL[ k ] > max_lag ) {
            pitchL[ k ] = max_lag;
        }
    }
}
