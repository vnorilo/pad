#include "pad_samples_sse2.h"
#include <smmintrin.h>
#include <immintrin.h>

#ifdef HAS_BIG_ENDIAN
#error AVX and big endian probably shouldn't coexist in a build :)
#endif

#define PAD_SAMPLES_AVX

namespace PAD{
	namespace Converter{
		template <> struct SampleVector<int32_t,8>{
			__m256i data;
			SampleVector(){}
			SampleVector(__m256i d):data(d){}
			SampleVector(const int32_t* mem):data(_mm256_loadu_si256((__m256i*)(mem))){}
			SampleVector(int32_t b) {data = _mm256_set_epi32(b,b,b,b,b,b,b,b);}
			void Write(int32_t* mem) {_mm256_storeu_si256((__m256i*)mem,data);}
			int32_t& operator[](unsigned i) {return data.m256i_i32[i];}
			int32_t operator[](unsigned i) const {return data.m256i_i32[i];}
		};

		template <> struct SampleVector<int16_t,8>{
			__m128i data;
			SampleVector(){}
			SampleVector(__m128i d):data(d){}
			SampleVector(const int16_t* mem) {data = _mm_set_epi16(mem[7],mem[6],mem[5],mem[4],mem[3],mem[2],mem[1],mem[0]);}
			SampleVector(int16_t b) {data = _mm_set_epi16(b,b,b,b,b,b,b,b);}
			void Write(int16_t *mem) {_mm_store_si128((__m128i*)mem,data);}
			int16_t& operator[](unsigned i) {return data.m128i_i16[i];}
			int16_t operator[](unsigned i) const {return data.m128i_i16[i];}
		};

		template <> struct SampleVector<float,8>{
			SampleVector(){}
			SampleVector(__m256 d):data(d){}
			SampleVector(const SampleVector<int32_t,8>& d):data(_mm256_cvtepi32_ps(d.data)){}
			SampleVector(const SampleVector<int16_t,8>& d):data(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(d.data))){}
			__m256 data;
			SampleVector(float b) {data = _mm256_set_ps(b,b,b,b,b,b,b,b);}
			SampleVector(const float* mem):data(_mm256_loadu_ps(mem)){}
			template <typename CVT> operator SampleVector<CVT,8>()
			{
				SampleVector<CVT,8> tmp;
				for(unsigned i(0);i<4;++i) tmp[i]=data.m256_f32[i];
				return tmp;
			}

			SampleVector<float,8> operator*(const SampleVector<float,8>& b) const { return _mm256_mul_ps(data,b.data); }
			SampleVector<float,8> operator/(const SampleVector<float,8>& b) const { return _mm256_div_ps(data,b.data); }

			operator SampleVector<int32_t,8>() { return _mm256_cvtps_epi32(data); }
			//			operator SampleVector<int16_t,8>() { return _mm256_cvtepi32_epi1 _mm256_cvtps_epi32(data); }

			float& operator[](unsigned i) {return data.m256_f32[i];}
			float operator[](unsigned i) const {return data.m256_f32[i];}
			void Write(float* mem) {_mm256_storeu_ps(mem,data);}
		};

		template <> struct SampleToHost<SampleVector<int32_t,8>,SampleVector<float,8>>{
			static void RoundAndClip(SampleVector<int32_t,8>& dst, const SampleVector<float,8>& src, const SampleVector<float,8>& hi, const SampleVector<float,8>& lo)
			{
				dst.data = _mm256_cvttps_epi32(
					_mm256_max_ps(
					_mm256_min_ps(
					_mm256_add_ps(src.data,_mm256_set_ps(0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f,0.5f)),
					hi.data),
					lo.data));
			}
		};

		template <> void Transpose<>(SampleVector<float,8> *v)
		{
			__m256 __t0, __t1, __t2, __t3, __t4, __t5, __t6, __t7;
			__m256 __tt0, __tt1, __tt2, __tt3, __tt4, __tt5, __tt6, __tt7;

			__t0 = _mm256_unpacklo_ps(v[0].data, v[1].data);
			__t1 = _mm256_unpackhi_ps(v[0].data, v[1].data);
			__t2 = _mm256_unpacklo_ps(v[2].data, v[3].data);
			__t3 = _mm256_unpackhi_ps(v[2].data, v[3].data);
			__t4 = _mm256_unpacklo_ps(v[4].data, v[5].data);
			__t5 = _mm256_unpackhi_ps(v[4].data, v[5].data);
			__t6 = _mm256_unpacklo_ps(v[6].data, v[7].data);
			__t7 = _mm256_unpackhi_ps(v[6].data, v[7].data);

			__tt0 = _mm256_shuffle_ps(__t0,__t2,_MM_SHUFFLE(1,0,1,0));
			__tt1 = _mm256_shuffle_ps(__t0,__t2,_MM_SHUFFLE(3,2,3,2));
			__tt2 = _mm256_shuffle_ps(__t1,__t3,_MM_SHUFFLE(1,0,1,0));
			__tt3 = _mm256_shuffle_ps(__t1,__t3,_MM_SHUFFLE(3,2,3,2));
			__tt4 = _mm256_shuffle_ps(__t4,__t6,_MM_SHUFFLE(1,0,1,0));
			__tt5 = _mm256_shuffle_ps(__t4,__t6,_MM_SHUFFLE(3,2,3,2));
			__tt6 = _mm256_shuffle_ps(__t5,__t7,_MM_SHUFFLE(1,0,1,0));
			__tt7 = _mm256_shuffle_ps(__t5,__t7,_MM_SHUFFLE(3,2,3,2));

			v[0].data = _mm256_permute2f128_ps(__tt0, __tt4, 0x20);
			v[1].data = _mm256_permute2f128_ps(__tt1, __tt5, 0x20);
			v[2].data = _mm256_permute2f128_ps(__tt2, __tt6, 0x20);
			v[3].data = _mm256_permute2f128_ps(__tt3, __tt7, 0x20);
			v[4].data = _mm256_permute2f128_ps(__tt0, __tt4, 0x31);
			v[5].data = _mm256_permute2f128_ps(__tt1, __tt5, 0x31);
			v[6].data = _mm256_permute2f128_ps(__tt2, __tt6, 0x31);
			v[7].data = _mm256_permute2f128_ps(__tt3, __tt7, 0x31);
		}
	}
}