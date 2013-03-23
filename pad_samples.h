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
			static SampleVector<E,N> Swap(const SampleVector<E,N>& _x)
			{
				SampleVector<E,N> x;
				for(unsigned i(0);i<N;++i) x[i] = Bytes<E>::Swap(x[i]);
				return x;
			}
		};

		template <typename HOST_FORMAT, typename CANONICAL_FORMAT, int NOMINAL_MINUS, int NOMINAL_PLUS, bool BIGENDIAN> struct HostSample {
			typedef HostSample<HOST_FORMAT,CANONICAL_FORMAT,NOMINAL_MINUS,NOMINAL_PLUS,BIGENDIAN> _myt;
			typedef HOST_FORMAT smp_t;
			HOST_FORMAT data;
			HostSample(){}

			HostSample(const HOST_FORMAT& constructFrom):data(constructFrom)
			{
				if (BIGENDIAN != SYSTEM_BIGENDIAN)
				{
					data = Bytes<HOST_FORMAT>::Swap(data);
				}
			}

			HostSample(const CANONICAL_FORMAT& convertFrom)
			{
				*this = convertFrom;
			}

			HostSample& operator=(const CANONICAL_FORMAT& convertFrom)
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
}