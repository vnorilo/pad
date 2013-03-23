#pragma once

#undef max
#undef min

#include <cstdint>
#include <cmath>
#include <cassert>
#include <numeric>
#include <algorithm>

#ifdef HAS_BIG_ENDIAN
#define SYSTEM_BIGENDIAN true
#else
#define SYSTEM_BIGENDIAN false
#endif

namespace PAD{
	namespace Converter{
		using namespace std;

		class int24_t{
			uint8_t bytes[3];
			operator int32_t()
			{
				union{
					uint8_t bytes[4];
					int32_t dword;
				} tmp;
				tmp.dword = 0x01020304;
				if (tmp.bytes[0] == 1)
				{
					tmp.bytes[0]=bytes[0];
					tmp.bytes[1]=bytes[1];
					tmp.bytes[2]=bytes[2];
					tmp.bytes[3]=0;
				}
				else if (tmp.bytes[0] == 4)
				{
					tmp.bytes[0]=0;
					tmp.bytes[1]=bytes[0];
					tmp.bytes[2]=bytes[1];
					tmp.bytes[3]=bytes[2];
				}
				else assert(0 && "Endian check failed");
				return tmp.dword;
			}

			int24_t(int32_t v)
			{
				union{
					uint8_t bytes[4];
					int32_t dword;
				} tmp;
				tmp.dword=0x01020304;
				if (tmp.bytes[0] == 1)
				{
					tmp.dword = v;
					bytes[0] = tmp.bytes[1];
					bytes[1] = tmp.bytes[2];
					bytes[2] = tmp.bytes[3];
				}
				else if (tmp.bytes[0] == 4)
				{
					tmp.dword = v;
					bytes[0] = tmp.bytes[0];
					bytes[1] = tmp.bytes[1];
					bytes[2] = tmp.bytes[2];
				}
				else assert(0 && "Endian check failed");
			}
		};

		template <typename S, int N> struct SampleVector{
			S data[N];
			SampleVector(){}
			SampleVector(const S* memory) {for(unsigned i(0);i<N;++i) data[i]=memory[i];}
			SampleVector(S broadcast) {for(unsigned i(0);i<N;++i) data[i]=broadcast;}

			template <typename FMT> SampleVector(const SampleVector<FMT,N>& src)
			{
				for(unsigned i(0);i<N;++i) data[i] = S(src[i]);
			}

			S& operator[](unsigned i) {return data[i];}
			S operator[](unsigned i) const {return data[i];}
			void Write(S* dest) {memcpy(dest,data,sizeof(S)*N);}

			SampleVector<S,N> operator*(SampleVector<S,N> b) const
			{
				for(unsigned i(0);i<N;++i) b[i]*=data[i];
				return b;
			}
		};

		/* generic clip & round */
		template <typename HOST, typename CANON> struct SampleToHost {
			static void RoundAndClip(HOST& dst, CANON src, CANON high_bound, CANON low_bound)
			{
				src = max(min(CANON(src + 0.5),high_bound),low_bound);
				dst = static_cast<HOST>(src);
			}
		};

		/* don't round floating point formats */
		template <> struct SampleToHost<float,float> {
			static void RoundAndClip(float& dst, float src, float hi, float lo)
			{
				dst = max(min(src,hi),lo);
			}
		};

		template <> struct SampleToHost<double,float> {
			static void RoundAndClip(double& dst, float src, float hi, float lo)
			{
				dst = max(min((double)src,(double)hi),(double)lo);
			}
		};

		template <typename HOST, int VEC> struct SampleToHost<SampleVector<HOST,VEC>,SampleVector<float,VEC>> {
			static void RoundAndClip(SampleVector<HOST,VEC>& dst, SampleVector<float,VEC> src, SampleVector<float,VEC> hi, SampleVector<float,VEC> lo)
			{
				for(unsigned i(0);i<VEC;++i)
				{
					SampleToHost<HOST,float>::RoundAndClip(dst[i],src[i],hi[i],lo[i]);
				}
			}
		};

		template <typename DATA> struct Bytes {
			static DATA Swap(const DATA& x)
			{
				union{
					uint8_t bytes[sizeof(DATA)];
					DATA word;
				} tmp;
				tmp.word = x;

				for(unsigned i(0);i<sizeof(DATA)/2;++i)
					swap(tmp.bytes[i],tmp.bytes[sizeof(DATA)-1-i]);

				return tmp.word;
			}
		};

		template <typename E, int N> struct Bytes<SampleVector<E,N>> {
			static SampleVector<E,N> Swap(SampleVector<E,N> x)
			{
				for(unsigned i(0);i<N;++i) x[i] = Bytes<E>::Swap(x[i]);
				return x;
			}
		};

		template <typename HOST_FORMAT, typename CANONICAL_FORMAT, int NOMINAL_MINUS, int NOMINAL_PLUS, bool BIGENDIAN> struct HostSample {
			typedef HostSample<HOST_FORMAT,CANONICAL_FORMAT,NOMINAL_MINUS,NOMINAL_PLUS,BIGENDIAN> _myt;
			typedef HOST_FORMAT smp_t;
			HOST_FORMAT data;
			HostSample(){}

			HostSample(HOST_FORMAT constructFrom):data(constructFrom)
			{
				if (BIGENDIAN != SYSTEM_BIGENDIAN)
				{
					data = Bytes<HOST_FORMAT>::Swap(data);
				}
			}

			HostSample(CANONICAL_FORMAT convertFrom)
			{
				*this = convertFrom;
			}

			HostSample& operator=(CANONICAL_FORMAT convertFrom)
			{
				/* float -> int */
				SampleToHost<HOST_FORMAT,CANONICAL_FORMAT>::RoundAndClip(
					data, convertFrom * CANONICAL_FORMAT(-NOMINAL_MINUS), CANONICAL_FORMAT(NOMINAL_PLUS), CANONICAL_FORMAT(NOMINAL_MINUS));

				if (BIGENDIAN != SYSTEM_BIGENDIAN)
				{
					data = Bytes<HOST_FORMAT>::Swap(data);
				}
				return *this;
			}

			operator CANONICAL_FORMAT() const
			{
				/* int -> float */
				HOST_FORMAT tmp(data);
				if (BIGENDIAN != SYSTEM_BIGENDIAN)
				{
					Bytes<HOST_FORMAT>::Swap(tmp);
				}

				return CANONICAL_FORMAT(tmp) * CANONICAL_FORMAT(-1.0/(double)NOMINAL_MINUS);
			}

			template <int N> static
				HostSample<SampleVector<HOST_FORMAT,N>,SampleVector<CANONICAL_FORMAT,N>,NOMINAL_MINUS,NOMINAL_PLUS,BIGENDIAN> ConstructVector()
			{
				return HostSample<SampleVector<HOST_FORMAT,N>,SampleVector<CANONICAL_FORMAT,N>,NOMINAL_MINUS,NOMINAL_PLUS,BIGENDIAN>();
			}

			template <int N> static
				HostSample<SampleVector<HOST_FORMAT,N>,SampleVector<CANONICAL_FORMAT,N>,NOMINAL_MINUS,NOMINAL_PLUS,BIGENDIAN> LoadVector(const _myt* ptr)
			{
				HostSample<SampleVector<HOST_FORMAT,N>,SampleVector<CANONICAL_FORMAT,N>,NOMINAL_MINUS,NOMINAL_PLUS,BIGENDIAN> tmp;
				const void *ptr1 = &ptr->data;
				const void *ptr2 = ptr;
				assert((const void*)&ptr->data == (const void*)ptr);
				tmp.data = &ptr->data;
				return tmp;
			}
		};
		template <int N, typename SMP>
		static void Transpose(SampleVector<SMP,N> *v)
		{
			for(unsigned i(0);i<N;++i)
				for(unsigned j(i+1);j<N;++j)
					swap(v[i][j],v[j][i]);
		}
	}

	using namespace Converter;

	template <typename SAMPLE> class ChannelConverter{
		static void DeInterleave4(const float *interleavedBuffer, SAMPLE **blockBuffers, unsigned frames, unsigned stride)
		{
			unsigned i(0);
			SAMPLE::smp_t *bb1((SAMPLE::smp_t*)blockBuffers[0]);
			SAMPLE::smp_t *bb2((SAMPLE::smp_t*)blockBuffers[1]);
			SAMPLE::smp_t *bb3((SAMPLE::smp_t*)blockBuffers[2]);
			SAMPLE::smp_t *bb4((SAMPLE::smp_t*)blockBuffers[3]);
			for(;i+4<=frames;i+=4)
			{
				SampleVector<float,4> mtx[4] = {
					interleavedBuffer + i * stride,
					interleavedBuffer + (i+1) * stride,
					interleavedBuffer + (i+2) * stride,
					interleavedBuffer + (i+3) * stride
				};

				Transpose(mtx);

				auto out1(SAMPLE::ConstructVector<4>());
				auto out2(SAMPLE::ConstructVector<4>());
				auto out3(SAMPLE::ConstructVector<4>());
				auto out4(SAMPLE::ConstructVector<4>());

				out1 = mtx[0];
				out2 = mtx[1];
				out3 = mtx[2];
				out4 = mtx[3];

				out1.data.Write(bb1 + i);
				out2.data.Write(bb2 + i);
				out3.data.Write(bb3 + i);
				out4.data.Write(bb4 + i);			
			}

			if (i < frames)
			{
				/* loop remainder */
				SAMPLE *offset1[2] = {blockBuffers[0] + i, blockBuffers[1] + i};
				DeInterleave2(interleavedBuffer + i * stride,offset1,frames-i,stride);
				SAMPLE *offset2[2] = {blockBuffers[2] + i, blockBuffers[3] + i};
				DeInterleave2(interleavedBuffer + i * stride + 2,offset2,frames-i,stride);
			}
		}

		static void DeInterleave2(const float *interleavedBuffer, SAMPLE **blockBuffers, unsigned frames, unsigned stride)
		{
			unsigned i(0);
			SAMPLE::smp_t *bb1((SAMPLE::smp_t*)blockBuffers[0]);
			SAMPLE::smp_t *bb2((SAMPLE::smp_t*)blockBuffers[1]);

			for(;i+2<=frames;i+=2)
			{

				SampleVector<float,2> mtx[2] = {
					interleavedBuffer + i * stride,
					interleavedBuffer + (i+1) * stride,
				};

				Transpose(mtx);

				auto out1(SAMPLE::ConstructVector<2>());
				auto out2(SAMPLE::ConstructVector<2>());

				out1 = mtx[0];
				out2 = mtx[1];

				out1.data.Write(bb1 + i);
				out2.data.Write(bb2 + i);
			}

			if (i < frames)
			{
				/* loop remainder */
				DeInterleave1(interleavedBuffer + i * stride, blockBuffers[0]+i, frames - i, stride);
				DeInterleave1(interleavedBuffer + i * stride + 1, blockBuffers[1]+i, frames - i, stride);
			}
		}

		static void DeInterleave1(const float *interleavedBuffer,SAMPLE* blockBuffer,unsigned frames, unsigned stride)
		{
			/* interleave 1 channel from bundle into destination */
			for(unsigned i(0);i<frames;++i)
			{
				blockBuffer[i] = interleavedBuffer[i*stride];
			}
		}

		static void Interleave4(float *interleavedBuffer, const SAMPLE **blockBuffers, unsigned frames, unsigned stride)
		{
			unsigned i(0);
			const SAMPLE* const bb1(blockBuffers[0]);
			const SAMPLE* const bb2(blockBuffers[1]);
			const SAMPLE* const bb3(blockBuffers[2]);
			const SAMPLE* const bb4(blockBuffers[3]);
			for(;i+4<=frames;i+=4)
			{
				SampleVector<float,4> mtx[4] = {
					SAMPLE::LoadVector<4>(bb1 + i),
					SAMPLE::LoadVector<4>(bb2 + i),
					SAMPLE::LoadVector<4>(bb3 + i),
					SAMPLE::LoadVector<4>(bb4 + i),
				};

				Transpose(mtx);

				mtx[0].Write(interleavedBuffer + i * stride);
				mtx[1].Write(interleavedBuffer + (i+1) * stride);
				mtx[2].Write(interleavedBuffer + (i+2) * stride);
				mtx[3].Write(interleavedBuffer + (i+3) * stride);
			}

			if (i < frames)
			{
				/* loop remainder */
				unsigned rem = frames - i;
				const SAMPLE *offset1[2] = {bb1 + i, bb2 + i};
				Interleave2(interleavedBuffer + i * stride,offset1,rem,stride);
				const SAMPLE *offset2[2] = {bb3 + i, bb4 + i};
				Interleave2(interleavedBuffer + i * stride + 2,offset2,rem,stride);
			}
		}

		static void Interleave2(float *interleavedBuffer, const SAMPLE **blockBuffers, unsigned frames, unsigned stride)
		{
			unsigned i(0);
			const SAMPLE* const bb1(blockBuffers[0]);
			const SAMPLE* const bb2(blockBuffers[1]);
			for(;i+2<=frames;i+=2)
			{
				SampleVector<float,2> mtx[2] = {
					SAMPLE::LoadVector<2>(bb1 + i),
					SAMPLE::LoadVector<2>(bb2 + i),
				};

				Transpose(mtx);

				mtx[0].Write(interleavedBuffer + i * stride);
				mtx[1].Write(interleavedBuffer + (i+1) * stride);
			}

			if (i < frames)
			{
				Interleave1(interleavedBuffer + i * stride, bb1+i, frames - i, stride);
				Interleave1(interleavedBuffer + i * stride + 1, bb2+i, frames - i, stride);
			}
		}

		static void Interleave1(float *interleavedBuffer, const SAMPLE* blockBuffer, unsigned frames, unsigned stride)
		{
			/* interleave 1 channel from bundle into destination */
			for(unsigned i(0);i<frames;++i)
			{
				interleavedBuffer[i*stride] = blockBuffer[i];
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
			else if (channels >= 4)
			{
				/* interleave 4 channels from bundle into destination */
				DeInterleave4(interleavedBuffer,blockBuffers,frames,stride);
				DeInterleaveVectored(interleavedBuffer + 4, blockBuffers + 4, frames, channels - 4, stride);
			}
			else if (channels >= 2)
			{
				/* interleave 2 channels from bundle into destination */
				DeInterleave2(interleavedBuffer,blockBuffers,frames,stride);
				DeInterleaveVectored(interleavedBuffer + 2, blockBuffers + 2, frames, channels - 2, stride);
			}
			else
			{
				DeInterleave1(interleavedBuffer,blockBuffers[0],frames,stride);
			}
		}

		static void InterleaveVectored(float *interleavedBuffer, const SAMPLE **blockBuffers, unsigned frames, unsigned channels, unsigned stride)
		{
			if (channels == 0) return;
			else if (channels >= 4)
			{
				/* interleave 4 channels from bundle into destination */
				Interleave4(interleavedBuffer,blockBuffers,frames,stride);
				InterleaveVectored(interleavedBuffer + 4, blockBuffers + 4, frames, channels - 4, stride);
			}
			else if (channels >= 2)
			{
				/* interleave 2 channels from bundle into destination */
				Interleave2(interleavedBuffer,blockBuffers,frames,stride);
				InterleaveVectored(interleavedBuffer + 2, blockBuffers + 2, frames, channels - 2, stride);
			}
			else
			{
				Interleave1(interleavedBuffer,blockBuffers[0],frames,stride);
			}
		}


	};
}