/* Copyright (c) 2018 Gregor Richards
 * Copyright (c) 2017 Mozilla */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

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
#include <string.h>
#include <stdio.h>
#include "rnnoise.h"

#define IO_FRAME_SIZE 2048
#define FRAME_SIZE 480

static float input_buffer[IO_FRAME_SIZE + FRAME_SIZE];
static float output_buffer[IO_FRAME_SIZE + FRAME_SIZE];

float buffered_rnnoise_process_frame(DenoiseState *st, int *out, const int *in) {
    // rnnnoise_process_frame has to be called with 480 samples
    // (ie. every 10ms), however, the buffered function may be called
    // with say 2048 samples, we should internally buffer the
    // input. Every 2048 samples received as input, we should produce
    // 2048 output though. However, 2048 is not divisible by 480.
    //
    // When we receive the first frame, we get 2048 samples, of which
    // we have four whole 480 sample frame and 128 samples left (2048
    // - 4*480). We have two options:
    //
    // 1. Since we cannot produce 2048 samples, we internally produce
    // 4*480 outputs but buffer them and wait for the next 2048
    // samples. From the new input buffer, we take (480-128) = 352
    // samples, combine with the previous 128, to get a full
    // frame. This time we can produce 5 output frames, from which we
    // can send out 2048 outputs, but buffer 352 output samples.  So,
    // we need to maintain two buffers and two offets for buffering
    // input and output. The latency of the system is ~40ms. First
    // time, we do not produce anything, just zero sampled output.
    //
    // 2. Second approach is to zero pad an input buffer with 352
    // samples. When 2048 samples arrive, we append into this zero
    // padded buffer, to produce 5 outputs, from which we can send out
    // 2048 samples, leaving 352 output samples in a buffer. When the
    // next input frame of 2048 samples arrive, we calculate 4 whole
    // outputs, buffer the rest of 128 samples in the input side, the
    // output now has 352+4*480 = 2272 samples. We send out the 2048
    // samples, buffer the rest.... and so on. This scheme has lesser
    // latency of only 352 samples ~ 7.3ms. Also, lesser buffering
    // requirements, I think, we only need 2048+480 sized buffer at
    // any point.
    //
    // So, let us try the second approach
    float x[FRAME_SIZE];
    float y[FRAME_SIZE];

    // input_offset is where we write input samples into
    static size_t input_write_offset = 352; // 480*5 - 2048
    static size_t input_read_offset = 0;

    // output_offset is where we read output samples from
    static size_t output_read_offset = 0;
    static size_t output_write_offset = 0;

    // write input samples into the input_buffer at input_offset
    // total size of input buffer: 2048 + 480 = 2528
    int input_frame_size = IO_FRAME_SIZE + FRAME_SIZE;

    for (size_t i = 0; i < IO_FRAME_SIZE; i++) {
        size_t idx = (input_write_offset + i) % input_frame_size;
        // printf("idx = %d\n", idx);
        input_buffer[idx] = in[i];
    }
    input_write_offset += IO_FRAME_SIZE;
    input_write_offset %= input_frame_size;

    // at this point, we have filled 5*480 samples in the
    // input_buffer. We need to call rnnoise_process with 480 samples
    // each. We will have to call rnnoise 5 times or 4 times, but that
    // depends on whether we already have some output samples. How do
    // we determine that?
    int n_samples = (input_write_offset - input_read_offset);
    if (n_samples < 0) {
        n_samples += input_frame_size;
    }
    size_t rnnoise_iter = n_samples/FRAME_SIZE;

    printf("input samples: %d, processing %d frames\n", n_samples, rnnoise_iter);

    for (size_t i = 0; i < rnnoise_iter; i++) {
        // fill x with one frame.
        for (size_t j = 0; j < FRAME_SIZE; j++) {
            // printf("i = %d, j = %d, input_buf idx = %d\n", i, j, input_read_offset+j);
            size_t idx = input_read_offset+j;
            idx %= input_frame_size;
            x[j] = input_buffer[idx];
        }

        input_read_offset += FRAME_SIZE;
        input_read_offset = input_read_offset % input_frame_size;

        // XXX replace with a dummy process that copies input to
        // output as the first step to check if the buffering scheme
        // is working.
        dummy_rnnoise_process_frame(st, x, x);

        for (size_t j = 0; j < FRAME_SIZE; j++) {
            size_t idx = output_write_offset+j;
            idx %= input_frame_size;
            output_buffer[idx] = x[j];
        }
        output_write_offset += FRAME_SIZE;
        output_write_offset = output_write_offset % input_frame_size;
    }

    // now copy 2048 samples from output_read_offet into *out buffer.
    for (size_t j = 0; j < IO_FRAME_SIZE; j++) {
        size_t idx = (output_read_offset + j) % input_frame_size;
        out[j] = output_buffer[idx];
    }
    output_read_offset = (output_read_offset + IO_FRAME_SIZE) % input_frame_size;
}

int main(int argc, char **argv) {
  int first = 1;
  float x[IO_FRAME_SIZE];
  FILE *f1, *fout;
  DenoiseState *st;
  st = rnnoise_create(NULL);
  if (argc!=3) {
    fprintf(stderr, "usage: %s <noisy speech> <output denoised>\n", argv[0]);
    return 1;
  }
  f1 = fopen(argv[1], "rb");
  fout = fopen(argv[2], "wb");

  // first zero everything
  memset(input_buffer, 0, (IO_FRAME_SIZE+FRAME_SIZE)*sizeof(int));
  memset(output_buffer, 0, (IO_FRAME_SIZE+FRAME_SIZE)*sizeof(int));

  int count = 0;
  while (1) {
    short tmp[IO_FRAME_SIZE];
    fread(tmp, sizeof(short), IO_FRAME_SIZE, f1);
    if (feof(f1)) {
        printf("processed %d frames of size %d bytes\n", count, IO_FRAME_SIZE);
        break;
    }
    count++;

    for (size_t i=0;i<IO_FRAME_SIZE;i++)
        x[i] = tmp[i];
    buffered_rnnoise_process_frame(st, x, x);
    for (size_t i=0;i<IO_FRAME_SIZE;i++)
        tmp[i] = x[i];
    // skip the first frame
    /* if (!first) */
    fwrite(tmp, sizeof(short), IO_FRAME_SIZE, fout);
    first = 0;
  }
  rnnoise_destroy(st);
  fclose(f1);
  fclose(fout);
  return 0;
}
