namespace PAD {

	using namespace Converter;
	template <typename SAMPLE> class ChannelConverter{
		template <int VEC> static void DeInterleave(const float *interleavedBuffer, SAMPLE **blockBuffers, unsigned frames, unsigned stride)
		{
			unsigned i(0);
			for(;i+4<=frames;i+=4)
			{
				SampleVector<float,4> mtx[VEC];
				for(unsigned j(0);j<VEC;++j) mtx[j] = interleavedBuffer + (i+j) * stride;
				
				Transpose(mtx);

				for(unsigned j(0);j<VEC;++j)
				{
					auto out(SAMPLE::ConstructVector<4>());
					out = mtx[j];
					out.data.Write((SAMPLE::smp_t*)blockBuffers[j]+i);
				}
			}

			if (i < frames)
			{
				/* loop remainder */
				SAMPLE *offset[VEC];
				for(unsigned j(0);j<VEC;++j) offset[j] = blockBuffers[j]+i;
				
				DeInterleave<(VEC+1)/2>(interleavedBuffer + i * stride,offset,frames-i,stride);
				DeInterleave<(VEC+1)/2>(interleavedBuffer + i * stride + VEC/2,offset+VEC/2,frames-i,stride);
			}
		}

		template <int VEC> static void Interleave(float *interleavedBuffer, const SAMPLE **blockBuffers, unsigned frames, unsigned stride)
		{
			unsigned i(0);
			const SAMPLE* bb[VEC];//={blockBuffers[0],blockBuffers[1],blockBuffers[2],blockBuffers[3]};
			for(unsigned i(0);i<VEC;++i) bb[i]=blockBuffers[i];
			for(;i+VEC<=frames;i+=VEC)
			{
				SampleVector<float,VEC> mtx[VEC];
				for(unsigned j(0);j<VEC;++j) 
					mtx[j] = SAMPLE::LoadVector<VEC>(bb[j] + i);

				Transpose(mtx);

				for(unsigned j(0);j<VEC;++j) 
					mtx[j].Write(interleavedBuffer + (i+j) * stride);
			}

			if (i < frames)
			{
				/* loop remainder */
				unsigned rem = frames - i;
				const SAMPLE *offset[VEC];
				for(unsigned j(0);j<VEC;++j) offset[j] = bb[j] + i;				
				Interleave<(VEC+1)/2>(interleavedBuffer + i * stride,offset,rem,stride);
				Interleave<(VEC+1)/2>(interleavedBuffer + i * stride + VEC/2,offset+VEC/2,rem,stride);
			}
		}

		public:
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

		static void DeInterleaveVectored(const float *interleavedBuffer, SAMPLE **blockBuffers, unsigned frames, unsigned channels, unsigned stride)
		{
			if (channels == 0) return;
			//if (channels >= 8)
			//{
			//	DeInterleave<8>(interleavedBuffer,blockBuffers,frames,stride);
			//	DeInterleaveVectored(interleavedBuffer + 8, blockBuffers + 8, frames, channels - 8, stride);
			//}
			else if (channels >= 4)
			{
				/* interleave 4 channels from bundle into destination */
				DeInterleave<4>(interleavedBuffer,blockBuffers,frames,stride);
				DeInterleaveVectored(interleavedBuffer + 4, blockBuffers + 4, frames, channels - 4, stride);
			}
			else if (channels >= 2)
			{
				/* interleave 2 channels from bundle into destination */
				DeInterleave<2>(interleavedBuffer,blockBuffers,frames,stride);
				DeInterleaveVectored(interleavedBuffer + 2, blockBuffers + 2, frames, channels - 2, stride);
			}
			else
			{
				DeInterleave<1>(interleavedBuffer,blockBuffers,frames,stride);
			}
		}

		static void InterleaveVectored(float *interleavedBuffer, const SAMPLE **blockBuffers, unsigned frames, unsigned channels, unsigned stride)
		{
			if (channels == 0) return;
			//if (channels >= 8)
			//{
			//	Interleave<8>(interleavedBuffer,blockBuffers,frames,stride);
			//	InterleaveVectored(interleavedBuffer + 8, blockBuffers + 8, frames, channels - 8, stride);
			//}
			if (channels >= 4)
			{
				/* interleave 4 channels from bundle into destination */
				Interleave<4>(interleavedBuffer,blockBuffers,frames,stride);
				InterleaveVectored(interleavedBuffer + 4, blockBuffers + 4, frames, channels - 4, stride);
			}
			else if (channels >= 2)
			{
				/* interleave 2 channels from bundle into destination */
				Interleave<2>(interleavedBuffer,blockBuffers,frames,stride);
				InterleaveVectored(interleavedBuffer + 2, blockBuffers + 2, frames, channels - 2, stride);
			}
			else
			{
				Interleave<1>(interleavedBuffer,blockBuffers,frames,stride);
			}
		}
	};
}