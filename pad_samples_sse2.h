#include <emmintrin.h>
#ifdef HAS_BIG_ENDIAN
#error SSE2 and big endian probably shouldnt coexist in a build :)
#endif

#define PAD_SAMPLES_SSE2
#ifdef PAD_SAMPLES_AVX
#error Include sse2 header before avx
#endif

namespace PAD{
	namespace Converter{
		template <> struct SampleVector<int32_t,4>{
			__m128i data;
			SampleVector(){}
			SampleVector(__m128i d):data(d){}
//			SampleVector(const int32_t* mem):data(_mm_load_si128((__m128i*)(mem))){}
			SampleVector(int32_t b) {data = _mm_set_epi32(b,b,b,b);}

			template <bool ALIGNED> void Write(int32_t* mem) {if (ALIGNED) _mm_store_si128((__m128i*)mem,data); else _mm_storeu_si128((__m128i*)mem,data);}
			template <bool ALIGNED> void Load(const int32_t* mem) { data = ALIGNED?_mm_load_si128((__m128i*)mem):_mm_loadu_si128((__m128i*)mem);}

#ifdef _MSC_VER
			int32_t& operator[](unsigned i) { return data.m128i_i32[i]; }
			int32_t operator[](unsigned i) const { return data.m128i_i32[i]; }
#else
			int32_t& operator[](unsigned i) { return  ((int32_t*)&data)[i]; }
			int32_t operator[](unsigned i) const {return  ((int32_t*)&data)[i];}
#endif
			SampleVector<int32_t,4> operator*(const SampleVector<int32_t,4>& b) {return _mm_mul_epu32(data,b.data);}
		};

		template <> struct SampleVector<int16_t,4>{
			__m128i data;
			SampleVector(){}
			SampleVector(__m128i d):data(d){}
			SampleVector(int16_t b) {data = _mm_set_epi16(0,0,0,0,b,b,b,b);}

			template <bool ALIGNED> void Write(int16_t* mem) {_mm_storel_epi64((__m128i*)mem,data);}
			template <bool ALIGNED> void Load(const int16_t* mem) { data = _mm_loadl_epi64((const __m128i*)mem);}

#ifdef _MSC_VER
			int16_t& operator[](unsigned i) { return data.m128i_i16[i]; }
			int16_t operator[](unsigned i) const {return data.m128i_i16[i];}
#else
			int16_t& operator[](unsigned i) {return  ((int16_t*)&data)[i];}
			int16_t operator[](unsigned i) const { return  ((int16_t*)&data)[i]; }
#endif

			SampleVector<int16_t,4> operator*(const SampleVector<int16_t,4>& b) {return _mm_mullo_epi16(data,b.data);}
		};

		template <> struct SampleVector<float,4>{
			SampleVector(){}
			SampleVector(__m128 d):data(d){}
			SampleVector(const SampleVector<int32_t,4>& d):data(_mm_cvtepi32_ps(d.data)){}
			SampleVector(const SampleVector<int16_t,4>& d):data(_mm_set_ps(d[3],d[2],d[1],d[0])) {}
			__m128 data;
            float *AsFloat() {return (float*)&data;}
			SampleVector(float b) {data = _mm_set_ps(b,b,b,b);}
//			SampleVector(const float* mem):data(_mm_load_ps(mem)){}
			template <typename CVT> operator SampleVector<CVT,4>()
			{
				SampleVector<CVT,4> tmp;
				for(unsigned i(0);i<4;++i) tmp[i]=AsFloat()[i];
				return tmp;
			}

			SampleVector<float,4> operator*(const SampleVector<float,4>& b) const { return _mm_mul_ps(data,b.data); }

			operator SampleVector<int32_t,4>() { return _mm_cvtps_epi32(data); }
			operator SampleVector<int16_t,4>() { return _mm_cvtps_epi32(data); }

#ifdef _MSC_VER
			float& operator[](unsigned i) { return data.m128_f32[i]; }
			float operator[](unsigned i) const { return data.m128_f32[i]; }
#else
			float& operator[](unsigned i) {return  ((float*)&data)[i];}
			float operator[](unsigned i) const {return  ((float*)&data)[i];}
#endif
			template <bool ALIGNED> void Write(float* mem) {if (ALIGNED) _mm_store_ps(mem,data); else _mm_storeu_ps(mem,data);}
			template <bool ALIGNED> void Load(const float* mem) { data = ALIGNED?_mm_load_ps(mem):_mm_loadu_ps(mem);}
		};

		template <> struct SampleToHost<SampleVector<int32_t,4>,SampleVector<float,4>>{
			static void RoundAndClip(SampleVector<int32_t,4>& dst, const SampleVector<float,4> &src, const SampleVector<float,4> &hi, const SampleVector<float,4> &lo)
			{
				dst.data = _mm_cvttps_epi32(
					_mm_max_ps(
					_mm_min_ps(
					_mm_add_ps(src.data,_mm_set_ps(0.5f,0.5f,0.5f,0.5f)),
					hi.data),
					lo.data));
			}
		};

		template <> struct SampleToHost<SampleVector<int16_t,4>,SampleVector<float,4>>{
			static void RoundAndClip(SampleVector<int16_t,4>& dst, const SampleVector<float,4> &src, const SampleVector<float,4> &hi, const SampleVector<float,4> &lo)
			{
				/* todo: check rounding mode in outer scope */
				dst.data = _mm_cvttps_epi32(
					_mm_max_ps(_mm_min_ps(_mm_add_ps(src.data,_mm_set_ps(0.5f,0.5f,0.5f,0.5f)),hi.data),
					lo.data));
			}
		};
		static void Transpose(SampleVector<float, 4> *v) {
			__m128i t0 = _mm_castps_si128(_mm_unpacklo_ps(v[0].data, v[1].data));
			__m128i t1 = _mm_castps_si128(_mm_unpacklo_ps(v[2].data, v[3].data));
			__m128i t2 = _mm_castps_si128(_mm_unpackhi_ps(v[0].data, v[1].data));
			__m128i t3 = _mm_castps_si128(_mm_unpackhi_ps(v[2].data, v[3].data));

			v[0] = _mm_castsi128_ps(_mm_unpacklo_epi64(t0, t1));
			v[1] = _mm_castsi128_ps(_mm_unpackhi_epi64(t0, t1));
			v[2] = _mm_castsi128_ps(_mm_unpacklo_epi64(t2, t3));
			v[3] = _mm_castsi128_ps(_mm_unpackhi_epi64(t2, t3));
		}
	}
}
