/* (C) 2007 Jean-Marc Valin, CSIRO
*/
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:
   
   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
   
   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
   
   - Neither the name of the Xiph.org Foundation nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.
   
   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include "bands.h"
#include "modes.h"
#include "vq.h"
#include "cwrs.h"

/** Applies a series of rotations so that pulses are spread like a two-sided
exponential. The effect of this is to reduce the tonal noise created by the
sparse spectrum resulting from the pulse codebook */
static void exp_rotation(celt_norm_t *X, int len, float theta, int dir, int stride, int iter)
{
   int i, k;
   float c, s;
   c = cos(theta);
   s = dir*sin(theta);
   for (k=0;k<iter;k++)
   {
      for (i=0;i<len-stride;i++)
      {
         float x1, x2;
         x1 = X[i];
         x2 = X[i+stride];
         X[i] = c*x1 - s*x2;
         X[i+stride] = c*x2 + s*x1;
      }
      for (i=len-2*stride-1;i>=0;i--)
      {
         float x1, x2;
         x1 = X[i];
         x2 = X[i+stride];
         X[i] = c*x1 - s*x2;
         X[i+stride] = c*x2 + s*x1;
      }
   }
}

/* Compute the amplitude (sqrt energy) in each of the bands */
void compute_band_energies(const CELTMode *m, celt_sig_t *X, celt_ener_t *bank)
{
   int i, c, B, C;
   const int *eBands = m->eBands;
   B = m->nbMdctBlocks;
   C = m->nbChannels;
   for (c=0;c<C;c++)
   {
      for (i=0;i<m->nbEBands;i++)
      {
         int j;
         float sum = 1e-10;
         for (j=B*eBands[i];j<B*eBands[i+1];j++)
            sum += SIG_SCALING_1*SIG_SCALING_1*X[j*C+c]*X[j*C+c];
         bank[i*C+c] = ENER_SCALING*sqrt(sum);
         /*printf ("%f ", bank[i*C+c]);*/
      }
   }
   /*printf ("\n");*/
}

/* Normalise each band such that the energy is one. */
void normalise_bands(const CELTMode *m, celt_sig_t *freq, celt_norm_t *X, celt_ener_t *bank)
{
   int i, c, B, C;
   const int *eBands = m->eBands;
   B = m->nbMdctBlocks;
   C = m->nbChannels;
   for (c=0;c<C;c++)
   {
      for (i=0;i<m->nbEBands;i++)
      {
         int j;
         float g = 1.f/(1e-10+ENER_SCALING_1*bank[i*C+c]*sqrt(C));
         for (j=B*eBands[i];j<B*eBands[i+1];j++)
            X[j*C+c] = NORM_SCALING*SIG_SCALING_1*freq[j*C+c]*g;
      }
   }
   for (i=B*C*eBands[m->nbEBands];i<B*C*eBands[m->nbEBands+1];i++)
      X[i] = 0;
}

void renormalise_bands(const CELTMode *m, celt_norm_t *X)
{
   VARDECL(celt_ener_t *tmpE);
   ALLOC(tmpE, m->nbEBands*m->nbChannels, celt_ener_t);
   compute_band_energies(m, X, tmpE);
   /* FIXME: This isn't right */
   normalise_bands(m, X, X, tmpE);
}

/* De-normalise the energy to produce the synthesis from the unit-energy bands */
void denormalise_bands(const CELTMode *m, celt_norm_t *X, celt_sig_t *freq, celt_ener_t *bank)
{
   int i, c, B, C;
   const int *eBands = m->eBands;
   B = m->nbMdctBlocks;
   C = m->nbChannels;
   for (c=0;c<C;c++)
   {
      for (i=0;i<m->nbEBands;i++)
      {
         int j;
         float g = ENER_SCALING_1*sqrt(C)*bank[i*C+c];
         for (j=B*eBands[i];j<B*eBands[i+1];j++)
            freq[j*C+c] = NORM_SCALING_1*SIG_SCALING*X[j*C+c] * g;
      }
   }
   for (i=B*C*eBands[m->nbEBands];i<B*C*eBands[m->nbEBands+1];i++)
      freq[i] = 0;
}


/* Compute the best gain for each "pitch band" */
void compute_pitch_gain(const CELTMode *m, celt_norm_t *X, celt_norm_t *P, float *gains, celt_ener_t *bank)
{
   int i, B;
   const int *eBands = m->eBands;
   const int *pBands = m->pBands;
   VARDECL(float *w);
   B = m->nbMdctBlocks*m->nbChannels;
   ALLOC(w, B*eBands[m->nbEBands], float);
   for (i=0;i<m->nbEBands;i++)
   {
      int j;
      for (j=B*eBands[i];j<B*eBands[i+1];j++)
         w[j] = bank[i]*ENER_SCALING_1;
   }

   
   for (i=0;i<m->nbPBands;i++)
   {
      float Sxy=0;
      float Sxx = 0;
      int j;
      float gain;
      for (j=B*pBands[i];j<B*pBands[i+1];j++)
      {
         Sxy += 1.f*X[j]*P[j]*w[j];
         Sxx += 1.f*X[j]*X[j]*w[j];
      }
      gain = Sxy/(1e-10*NORM_SCALING*NORM_SCALING+Sxx);
      if (gain > 1.f)
         gain = 1.f;
      if (gain < 0.0f)
         gain = 0.0f;
      /* We need to be a bit conservative, otherwise residual doesn't quantise well */
      gain *= .9f;
      gains[i] = gain;
      /*printf ("%f ", 1-sqrt(1-gain*gain));*/
   }
   /*if(rand()%10==0)
   {
      for (i=0;i<m->nbPBands;i++)
         printf ("%f ", 1-sqrt(1-gains[i]*gains[i]));
      printf ("\n");
   }*/
   for (i=B*pBands[m->nbPBands];i<B*pBands[m->nbPBands+1];i++)
      P[i] = 0;
}

/* Apply the (quantised) gain to each "pitch band" */
void pitch_quant_bands(const CELTMode *m, celt_norm_t *X, celt_norm_t *P, float *gains)
{
   int i, B;
   const int *pBands = m->pBands;
   B = m->nbMdctBlocks*m->nbChannels;
   for (i=0;i<m->nbPBands;i++)
   {
      int j;
      for (j=B*pBands[i];j<B*pBands[i+1];j++)
         P[j] *= gains[i];
      /*printf ("%f ", gain);*/
   }
   for (i=B*pBands[m->nbPBands];i<B*pBands[m->nbPBands+1];i++)
      P[i] = 0;
}


/* Quantisation of the residual */
void quant_bands(const CELTMode *m, celt_norm_t *X, celt_norm_t *P, float *W, int total_bits, ec_enc *enc)
{
   int i, j, B, bits;
   const int *eBands = m->eBands;
   float alpha = .7;
   VARDECL(celt_norm_t *norm);
   VARDECL(int *pulses);
   VARDECL(int *offsets);
   
   B = m->nbMdctBlocks*m->nbChannels;
   
   ALLOC(norm, B*eBands[m->nbEBands+1], celt_norm_t);
   ALLOC(pulses, m->nbEBands, int);
   ALLOC(offsets, m->nbEBands, int);

   for (i=0;i<m->nbEBands;i++)
      offsets[i] = 0;
   /* Use a single-bit margin to guard against overrunning (make sure it's enough) */
   bits = total_bits - ec_enc_tell(enc, 0) - 1;
   compute_allocation(m, offsets, bits, pulses);
   
   /*printf("bits left: %d\n", bits);
   for (i=0;i<m->nbEBands;i++)
      printf ("%d ", pulses[i]);
   printf ("\n");*/
   /*printf ("%d %d\n", ec_enc_tell(enc, 0), compute_allocation(m, m->nbPulses));*/
   for (i=0;i<m->nbEBands;i++)
   {
      int q;
      float theta, n;
      q = pulses[i];
      /*Scale factor of .0625f is just there to prevent overflows in fixed-point
       (has no effect on float)*/
      n = .0625f*sqrt(B*(eBands[i+1]-eBands[i]));
      theta = .007*(B*(eBands[i+1]-eBands[i]))/(.1f+q);

      /* If pitch isn't available, use intra-frame prediction */
      if (eBands[i] >= m->pitchEnd || q<=0)
      {
         q -= 1;
         alpha = 0;
         if (q<0)
            intra_fold(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), norm, P+B*eBands[i], B, eBands[i], eBands[m->nbEBands+1]);
         else
            intra_prediction(X+B*eBands[i], W+B*eBands[i], B*(eBands[i+1]-eBands[i]), q, norm, P+B*eBands[i], B, eBands[i], enc);
      } else {
         alpha = .7;
      }
      
      if (q > 0)
      {
         exp_rotation(P+B*eBands[i], B*(eBands[i+1]-eBands[i]), theta, -1, B, 8);
         exp_rotation(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), theta, -1, B, 8);
         alg_quant(X+B*eBands[i], W+B*eBands[i], B*(eBands[i+1]-eBands[i]), q, P+B*eBands[i], alpha, enc);
         exp_rotation(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), theta, 1, B, 8);
      }
      for (j=B*eBands[i];j<B*eBands[i+1];j++)
         norm[j] = X[j] * n;
   }
   for (i=B*eBands[m->nbEBands];i<B*eBands[m->nbEBands+1];i++)
      X[i] = 0;
}

/* Decoding of the residual */
void unquant_bands(const CELTMode *m, celt_norm_t *X, celt_norm_t *P, int total_bits, ec_dec *dec)
{
   int i, j, B, bits;
   const int *eBands = m->eBands;
   float alpha = .7;
   VARDECL(celt_norm_t *norm);
   VARDECL(int *pulses);
   VARDECL(int *offsets);
   
   B = m->nbMdctBlocks*m->nbChannels;
   
   ALLOC(norm, B*eBands[m->nbEBands+1], celt_norm_t);
   ALLOC(pulses, m->nbEBands, int);
   ALLOC(offsets, m->nbEBands, int);

   for (i=0;i<m->nbEBands;i++)
      offsets[i] = 0;
   /* Use a single-bit margin to guard against overrunning (make sure it's enough) */
   bits = total_bits - ec_dec_tell(dec, 0) - 1;
   compute_allocation(m, offsets, bits, pulses);

   for (i=0;i<m->nbEBands;i++)
   {
      int q;
      float theta, n;
      q = pulses[i];
      /*Scale factor of .0625f is just there to prevent overflows in fixed-point
      (has no effect on float)*/
      n = .0625f*sqrt(B*(eBands[i+1]-eBands[i]));
      theta = .007*(B*(eBands[i+1]-eBands[i]))/(.1f+q);

      /* If pitch isn't available, use intra-frame prediction */
      if (eBands[i] >= m->pitchEnd || q<=0)
      {
         q -= 1;
         alpha = 0;
         if (q<0)
            intra_fold(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), norm, P+B*eBands[i], B, eBands[i], eBands[m->nbEBands+1]);
         else
            intra_unquant(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), q, norm, P+B*eBands[i], B, eBands[i], dec);
      } else {
         alpha = .7;
      }
      
      if (q > 0)
      {
         exp_rotation(P+B*eBands[i], B*(eBands[i+1]-eBands[i]), theta, -1, B, 8);
         alg_unquant(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), q, P+B*eBands[i], alpha, dec);
         exp_rotation(X+B*eBands[i], B*(eBands[i+1]-eBands[i]), theta, 1, B, 8);
      }
      for (j=B*eBands[i];j<B*eBands[i+1];j++)
         norm[j] = X[j] * n;
   }
   for (i=B*eBands[m->nbEBands];i<B*eBands[m->nbEBands+1];i++)
      X[i] = 0;
}

void stereo_mix(const CELTMode *m, celt_norm_t *X, celt_ener_t *bank, int dir)
{
   int i, B, C;
   const int *eBands = m->eBands;
   B = m->nbMdctBlocks;
   C = m->nbChannels;
   for (i=0;i<m->nbEBands;i++)
   {
      int j;
      float left, right;
      float a1, a2;
      left = bank[i*C];
      right = bank[i*C+1];
      a1 = left/sqrt(.01+left*left+right*right);
      a2 = dir*right/sqrt(.01+left*left+right*right);
      for (j=B*eBands[i];j<B*eBands[i+1];j++)
      {
         float r, l;
         l = X[j*C];
         r = X[j*C+1];         
         X[j*C] = a1*l + a2*r;
         X[j*C+1] = a1*r - a2*l;
      }
   }
   for (i=B*C*eBands[m->nbEBands];i<B*C*eBands[m->nbEBands+1];i++)
      X[i] = 0;

}
