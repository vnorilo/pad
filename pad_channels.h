namespace PAD {

	using namespace Converter;
	template <typename SAMPLE> class ChannelConverter{
		template <int VEC, bool ALIGN_I, bool ALIGN_B> static void DeInterleaveBundle(const float *interleavedBuffer, SAMPLE **blockBuffers, unsigned frames, unsigned stride)
		{
			unsigned i(0);
			for(;i+VEC<=frames;i+=VEC)
			{
				SampleVector<float,VEC> mtx[VEC];
				for(unsigned j(0);j<VEC;++j) mtx[j].template Load<ALIGN_I>(interleavedBuffer + (i+j) * stride);
				
				Transpose(mtx);

				for(unsigned j(0);j<VEC;++j)
				{
					auto tmp = SAMPLE::template ConstructVector<VEC>(blockBuffers[0]);
					tmp = mtx[j];
					tmp.data.template Write<ALIGN_B>((typename SAMPLE::smp_t*)blockBuffers[j]+i);
				}
			}

			if (i < frames)
			{
				/* loop remainder */
				SAMPLE *offset[VEC];
				for(unsigned j(0);j<VEC;++j) offset[j] = blockBuffers[j]+i;
				
				DeInterleaveBundle<(VEC+1)/2,ALIGN_I,ALIGN_B>(interleavedBuffer + i * stride,offset,frames-i,stride);
				DeInterleaveBundle<(VEC+1)/2,ALIGN_I,ALIGN_B>(interleavedBuffer + i * stride + VEC/2,offset+VEC/2,frames-i,stride);
			}
		}

		template <int VEC, bool ALIGN_I, bool ALIGN_B> static void InterleaveBundle(float *interleavedBuffer, const SAMPLE **blockBuffers, unsigned frames, unsigned stride)
		{
			unsigned i(0);
			const SAMPLE* bb[VEC];//={blockBuffers[0],blockBuffers[1],blockBuffers[2],blockBuffers[3]};
			for(i=0;i<VEC;++i) bb[i]=blockBuffers[i];
			for(i=0;i+VEC<=frames;i+=VEC)
			{
				SampleVector<float,VEC> mtx[VEC];
                SAMPLE fmt;
				for(unsigned j(0);j<VEC;++j) 
					mtx[j] = SAMPLE::template LoadVector<VEC,ALIGN_B>(bb[j] + i);

				Transpose(mtx);

				for(unsigned j(0);j<VEC;++j) 
					mtx[j].template Write<ALIGN_I>(interleavedBuffer + (i+j) * stride);
			}

			if (i < frames)
			{
				/* loop remainder */
				unsigned rem = frames - i;
				const SAMPLE *offset[VEC];
				for(unsigned j(0);j<VEC;++j) offset[j] = bb[j] + i;				
				InterleaveBundle<(VEC+1)/2,ALIGN_I,ALIGN_B>(interleavedBuffer + i * stride,offset,rem,stride);
				InterleaveBundle<(VEC+1)/2,ALIGN_I,ALIGN_B>(interleavedBuffer + i * stride + VEC/2,offset+VEC/2,rem,stride);
			}
		}

		static void DeInterleaveFallback(const float *interleavedBuffer, SAMPLE **blockBuffers, unsigned frames, unsigned channels, unsigned stride)
		{
			for(unsigned k(0);k<channels;++k)
				for(unsigned i(0);i<frames;++i)
					blockBuffers[k][i]=interleavedBuffer[i*stride+k];
		}

		static void InterleaveFallback(float *interleavedBuffer, const SAMPLE **blockBuffers, unsigned frames, unsigned channels, unsigned stride)
		{
			for(unsigned k(0);k<channels;++k)
				for(unsigned i(0);i<frames;++i)
					interleavedBuffer[i*stride+k]=blockBuffers[k][i];
		}

		template <bool AI, bool AB>
		static void DeInterleaveVectored(const float *interleavedBuffer, SAMPLE **blockBuffers, unsigned frames, unsigned channels, unsigned stride)
		{
			if (channels == 0) return;
			else if (channels >= 4)
			{
				/* interleave 4 channels from bundle into destination */
				DeInterleaveBundle<4,AI,AB>(interleavedBuffer,blockBuffers,frames,stride);
				DeInterleaveVectored<AI,AB>(interleavedBuffer + 4, blockBuffers + 4, frames, channels - 4, stride);
			}
			else if (channels >= 2)
			{
				/* interleave 2 channels from bundle into destination */
				DeInterleaveBundle<2,AI,AB>(interleavedBuffer,blockBuffers,frames,stride);
				DeInterleaveVectored<AI,AB>(interleavedBuffer + 2, blockBuffers + 2, frames, channels - 2, stride);
			}
			else
			{
				DeInterleaveBundle<1,false,false>(interleavedBuffer,blockBuffers,frames,stride);
			}
		}

		template <bool AI, bool AB>
		static void InterleaveVectored(float *interleavedBuffer, const SAMPLE **blockBuffers, unsigned frames, unsigned channels, unsigned stride)
		{
			if (channels == 0) return;
			if (channels >= 4)
			{
				/* interleave 4 channels from bundle into destination */
				InterleaveBundle<4,AI,AB>(interleavedBuffer,blockBuffers,frames,stride);
				InterleaveVectored<AI,AB>(interleavedBuffer + 4, blockBuffers + 4, frames, channels - 4, stride);
			}
			else if (channels >= 2)
			{
				/* interleave 2 channels from bundle into destination */
				InterleaveBundle<2,false,false>(interleavedBuffer,blockBuffers,frames,stride);
				InterleaveVectored<AI,AB>(interleavedBuffer + 2, blockBuffers + 2, frames, channels - 2, stride);
			}
			else
			{
				InterleaveBundle<1,false,false>(interleavedBuffer,blockBuffers,frames,stride);
			}
		}
	public:
		static void Interleave(float *interleavedBuffer, const SAMPLE **blockBuffers, unsigned frames, unsigned channels, unsigned stride)
		{
			/* are all block buffers aligned to 16 byte boundaries? */
			bool ai(true),ab(true);
			for(unsigned i(0);i<channels;++i)
			{
				intptr_t align = intptr_t(blockBuffers[i]);
				if ((align&15) != 0) 
				{
					ab = false;break;
				}
			}

			/* is the interleaved buffer and the stride 16-byte aligned? */
			intptr_t align = intptr_t(interleavedBuffer);
			ai = (align&15) == 0 && (stride % 4) == 0;

			/* specialize according to alignment properties of interleaved and block buffers */
			if (ai)
			{
				if (ab) InterleaveVectored<true,true>(interleavedBuffer,blockBuffers,frames,channels,stride);
				else InterleaveVectored<true,false>(interleavedBuffer,blockBuffers,frames,channels,stride);
			}
			else
			{
				if (ab) InterleaveVectored<false,true>(interleavedBuffer,blockBuffers,frames,channels,stride);
				else InterleaveVectored<false,false>(interleavedBuffer,blockBuffers,frames,channels,stride);
			}
		}
		static void DeInterleave(const float *interleavedBuffer, SAMPLE **blockBuffers, unsigned frames, unsigned channels, unsigned stride)
		{
			/* are all block buffers aligned to 16 byte boundaries? */
			bool ai(true),ab(true);
			for(unsigned i(0);i<channels;++i)
			{
				intptr_t align = intptr_t(blockBuffers[i]);
				if ((align&15) != 0) 
				{
					ab = false;
					break;
				}
			}

			/* is the interleaved buffer and the stride 16-byte aligned? */
			intptr_t align = intptr_t(interleavedBuffer);
			ai = (align&15) == 0 && (stride % 4) == 0;

			/* specialize according to alignment properties of interleaved and block buffers */
			if (ai)
			{
				if (ab) DeInterleaveVectored<true,true>(interleavedBuffer,blockBuffers,frames,channels,stride);
				else DeInterleaveVectored<true,false>(interleavedBuffer,blockBuffers,frames,channels,stride);
			}
			else
			{
				if (ab) DeInterleaveVectored<false,true>(interleavedBuffer,blockBuffers,frames,channels,stride);
				else DeInterleaveVectored<false,false>(interleavedBuffer,blockBuffers,frames,channels,stride);
			}
		}
	};
}